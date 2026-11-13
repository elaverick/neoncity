#include "city_internal.h"
#include "vendor_sites.h"

/* ===========================================================================
 * Where the street vendors stand (vendor_sites.h). A block's sites are planned
 * whole: its spaces (city_block_spaces) in order, then a court if the yards
 * behind its buildings leave one, then its kerbs. Each space is lined with
 * rows of sites; a row walks along its back line, trying a site of a drawn
 * width and depth, keeping it if it passes (candidate), and moving on.
 * ========================================================================= */

enum { GK_NONE = 0, GK_SIDEWALK, GK_LOT, GK_DROP };

#define MAX_BLOCK_SPOTS 160
#define MAX_KEEP        24
#define SAMPLE          0.45   /* the most between the points a site is checked at (m) */
#define KERB_END        6.0    /* a kerb pitch keeps this far from its edge's ends (the corners) */
#define KERB_CROSSING   2.5    /* ...and from a drop kerb (the crossing's waiting space) */
#define KERB_LAMP       0.5    /* ...and this far from a lamp post's pole */
#define KERB_BACK       (KERB_STONE_WIDTH + 0.12)   /* a kerb pitch's back off the kerb line */
#define DOOR_KEEP       0.6    /* a door's way is kept this much wider than the door each side (as props.c) */
#define DOOR_REACH      2.2    /* ...and out this far */
#define CEIL_MIN        2.6f   /* the lowest overhead a site may stand under */
#define OPEN_SKY        8.0f   /* anything higher overhead leaves it under the open sky */
#define COURT_CELL      0.5    /* the yards' raster */
#define COURT_MIN       10.0   /* a court is at least this square (m) */
/* SF3 (the user, on the first board): the freestanding vendors down to about a third, the ones in the lanes and
 * under their vaults kept, the ones along the pavements whittled down (the shops' bays and the buildings' corners
 * take their place): the kerb pitches at this share of their rate, the aprons filled this often, a few each */
#define KERB_SHARE      0.12f
#define APRON_SHARE     0.15f
#define APRON_MOST      2

/* A rectangle in its own frame: centre c, unit axes t and n, half extents. */
typedef struct OBB { DVec2 c, t, n; double hu, hn; } OBB;

typedef struct SGen {
    ChunkGen   *g;
    int         bi;
    const Block *bk;
    int         bl[256], nbl;           /* the block's buildings in g */
    DropKerb    dk[MAX_BLOCK_DROPS];
    int         nd;
    Lamp        lamp[96];
    int         nl;
    CitySpace   sp[MAX_BLOCK_SPACES + 1];
    int         nsp;
    VendorSpot  spot[MAX_BLOCK_SPOTS];
    OBB         box[MAX_BLOCK_SPOTS], way[MAX_BLOCK_SPOTS];   /* each site's rectangle and the way in front of it */
    int         n;
    OBB         keep[MAX_KEEP];         /* ways kept clear: into a court */
    int         nkeep;
    float       aff;
    CityVault   vault[MAX_BLOCK_VAULTS]; /* SF2: the vaults over its lanes */
    int         nvault;
} SGen;

const char *vendor_space_name(int space) {
    static const char *const N[SPACE_CORNER + 1] = { "apron", "lane", "pocket market", "plaza", "court", "kerb", "shop bay", "building corner" };
    return space >= 0 && space <= SPACE_CORNER ? N[space] : "?";
}

static int site_kind(int space) {
    switch (space) {
    case SPACE_APRON:  return VSITE_APRON;
    case SPACE_LANE:   return VSITE_LANE;
    case SPACE_POCKET: return VSITE_POCKET;
    case SPACE_PLAZA:  return VSITE_PLAZA;
    case SPACE_COURT:  return VSITE_COURT;
    case SPACE_SHOP:   return VSITE_SHOP;
    case SPACE_CORNER: return VSITE_CORNER;
    default:           return VSITE_KERB;
    }
}

/* ---------------------------------------------------------- geometry --- */

static double obb_reach(const OBB *a, DVec2 ax) { return a->hu * fabs(ddot(a->t, ax)) + a->hn * fabs(ddot(a->n, ax)); }

/* Whether two rectangles come within gap of each other (separating axes). */
static int obb_hit(const OBB *a, const OBB *b, double gap) {
    const DVec2 ax[4] = { a->t, a->n, b->t, b->n };
    DVec2 d = dsub(b->c, a->c);
    for (int k = 0; k < 4; ++k)
        if (fabs(ddot(d, ax[k])) >= obb_reach(a, ax[k]) + obb_reach(b, ax[k]) + gap) return 0;
    return 1;
}

/* A site with its back's middle at B, its front f: its rectangle, and the way
 * `walk` deep in front of it (a little wider than the site). */
static OBB site_box(DVec2 B, DVec2 f, double w, double d) {
    OBB o;
    o.t = dv(f.z, -f.x); o.n = f;
    o.c = dadd(B, dscale(f, d * 0.5));
    o.hu = w * 0.5; o.hn = d * 0.5;
    return o;
}
static OBB way_box(DVec2 B, DVec2 f, double w, double d, double walk) {
    OBB o = site_box(dadd(B, dscale(f, d)), f, w + 0.6, walk);
    return o;
}

void vendor_spot_corners(const VendorSpot *s, double (*v)[2]) {
    double rx = s->fz, rz = -s->fx, hw = s->site.w * 0.5, d = s->site.d;
    static const int su[4] = { 1, -1, -1, 1 }, sd[4] = { 0, 0, 1, 1 };
    for (int k = 0; k < 4; ++k) {
        v[k][0] = s->x + rx * hw * su[k] + s->fx * d * sd[k];
        v[k][1] = s->z + rz * hw * su[k] + s->fz * d * sd[k];
    }
}

int vendor_spot_contains(const VendorSpot *s, double x, double z, double margin, double front) {
    double dx = x - s->x, dz = z - s->z;
    double a = dx * s->fz - dz * s->fx, b = dx * s->fx + dz * s->fz;
    return fabs(a) < s->site.w * 0.5 + margin && b > -margin && b < s->site.d + front;
}

/* ------------------------------------------------------------ the block --- */

static int ground_at(const SGen *S, double x, double z) {
    const Block *bk = S->bk;
    if (x < bk->x0 || x > bk->x1 || z < bk->z0 || z > bk->z1 || !poly_contains(&bk->curb, x, z, 0.0)) return GK_NONE;
    if (bk->lot.n && poly_contains(&bk->lot, x, z, 0.0)) return GK_LOT;
    if (!poly_contains(&bk->curb, x, z, -KERB_STONE_WIDTH)) return GK_DROP;   /* the kerb stones */
    for (int i = 0; i < S->nd; ++i)
        if (city_drop_contains(&S->dk[i], x, z, 0.1)) return GK_DROP;
    return GK_SIDEWALK;
}

static int in_plot(const Building *b, double x, double z, double margin, float *lx, float *lz) {
    double dx = x - b->ox, dz = z - b->oz;
    double px = b->c * dx + b->s * dz, pz = -b->s * dx + b->c * dz;
    *lx = (float)px; *lz = (float)pz;
    return px > b->foot.x0 - margin && px < b->foot.x1 + margin && pz > b->foot.z0 - margin && pz < b->foot.z1 + margin;
}

/* Whether a circle of radius rad at (x, z) runs into a wall standing at street
 * level, a column or a plant's fence of the block's buildings (as props.c). */
static int solid_ex(SGen *S, double x, double z, float rad, int cols) {
    ChunkGen *g = S->g;
    for (int q = 0; q < S->nbl; ++q) {
        int i = S->bl[q];
        float lx, lz;
        if (!in_plot(&g->bld[i], x, z, rad + 0.6, &lx, &lz)) continue;
        const BGraph *bg = chunk_gen_graph(g, i);
        if (bgen_industrial(bg)) {
            const float *f = bg->fence;
            int inside = lx > f[0] - rad && lx < f[2] + rad && lz > f[1] - rad && lz < f[3] + rad;
            int within = lx > f[0] + rad && lx < f[2] - rad && lz > f[1] + rad && lz < f[3] - rad;
            if (inside && !within) return 1;
        }
        float ro = relief_out(bg);   /* a curtain wall's piers stand out this far at the street */
        for (int si = 0; si < bg->nsec; ++si) {
            const BSection *s = &bg->sec[si];
            if (s->y0 > SIDEWALK_HEIGHT + 0.5f || bsec_open(s)) continue;
            float r = rad + (ro > 0.0f && bsec_grounded(s) && relief_on(bg, s) ? ro + 0.02f : 0.0f);
            if (bfoot_contains(&s->foot, lx, lz) || bfoot_boundary_dist(&s->foot, lx, lz, NULL, NULL) < r) return 1;
        }
        for (int k = 0; k < bg->nsup && cols; ++k) {
            const BSupport *sp = &bg->sup[k];
            if (sp->type != SUP_COLUMN && sp->type != SUP_STRUT && sp->type != SUP_LEG) continue;
            float h = sp->size * 0.5f + rad;
            if (sp->y0 <= SIDEWALK_HEIGHT + 0.5f && fabsf(sp->x0 - lx) < h && fabsf(sp->z0 - lz) < h) return 1;
            if (sp->y1 <= SIDEWALK_HEIGHT + 0.5f && fabsf(sp->x1 - lx) < h && fabsf(sp->z1 - lz) < h) return 1;
        }
    }
    return 0;
}

static int solid(SGen *S, double x, double z, float rad) { return solid_ex(S, x, z, rad, 1); }

/* Whether a column, strut or leg of the block's buildings standing at street level, or a plant's fence, meets
 * rectangle o, grown by gap (exactly: a sampled site can straddle a thin column or fence between its samples). */
static int support_hit(SGen *S, const OBB *o, double gap) {
    ChunkGen *g = S->g;
    for (int q = 0; q < S->nbl; ++q) {
        const Building *b = &g->bld[S->bl[q]];
        const BGraph *bg = chunk_gen_graph(g, S->bl[q]);
        for (int k = 0; k < bg->nsup; ++k) {
            const BSupport *sp = &bg->sup[k];
            if (sp->type != SUP_COLUMN && sp->type != SUP_STRUT && sp->type != SUP_LEG) continue;
            for (int e = 0; e < 2; ++e) {
                if ((e ? sp->y1 : sp->y0) > SIDEWALK_HEIGHT + 0.5f) continue;
                OBB c;
                c.c = to_world(b, e ? sp->x1 : sp->x0, e ? sp->z1 : sp->z0);
                c.t = dv(b->c, b->s); c.n = dv(-b->s, b->c);
                c.hu = c.hn = 0.5 * sp->size;
                if (obb_hit(o, &c, gap)) return 1;
            }
        }
        if (bgen_industrial(bg)) {   /* a plant's fence: a thin strip a sampled site can straddle */
            FenceRun run[INDUSTRY_MAX_FENCE];
            const int nr = industry_fence(bg, run, INDUSTRY_MAX_FENCE);
            for (int i = 0; i < nr; ++i) {
                const FenceRun *r = &run[i];
                const double len = hypot(r->x1 - r->x0, r->z1 - r->z0);
                if (len < 1e-3) continue;
                OBB c;
                c.c = to_world(b, 0.5 * (r->x0 + r->x1), 0.5 * (r->z0 + r->z1));
                const DVec2 a = to_world(b, r->x0, r->z0), e = to_world(b, r->x1, r->z1);
                c.t = dscale(dsub(e, a), 1.0 / len); c.n = dv(-c.t.z, c.t.x);
                c.hu = 0.5 * len; c.hn = 0.08;
                if (obb_hit(o, &c, gap)) return 1;
            }
        }
    }
    return 0;
}

/* Whether (x, z) lies in a door's way: from its face out DOOR_REACH, DOOR_KEEP wider each side. */
static int in_doorway(SGen *S, double x, double z) {
    ChunkGen *g = S->g;
    for (int q = 0; q < S->nbl; ++q) {
        int i = S->bl[q];
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
            if (fabsf(u - d->u) < d->width * 0.5f + (float)DOOR_KEEP && o > -0.2f && o < (float)DOOR_REACH) return 1;
        }
    }
    return 0;
}

/* The underside of the lowest section of the block's buildings standing over
 * (x, z) (world Y), or 1e30. */
static float overhead(SGen *S, double x, double z) {
    ChunkGen *g = S->g;
    float best = 1e30f;
    for (int q = 0; q < S->nbl; ++q) {
        int i = S->bl[q];
        float lx, lz;
        if (!in_plot(&g->bld[i], x, z, 0.05, &lx, &lz)) continue;
        const BGraph *bg = chunk_gen_graph(g, i);
        float base = building_base(&g->bld[i]);
        for (int si = 0; si < bg->nsec; ++si) {
            const BSection *s = &bg->sec[si];
            if (s->y0 <= SIDEWALK_HEIGHT + 0.5f || !bfoot_contains(&s->foot, lx, lz)) continue;
            best = minf(best, base + s->y0);
        }
    }
    return best;
}

static int lamp_near(const SGen *S, double x, double z, double r) {
    for (int i = 0; i < S->nl; ++i) {
        double dx = x - S->lamp[i].x, dz = z - S->lamp[i].z, k = 0.22 + r;
        if (dx * dx + dz * dz < k * k) return 1;
    }
    return 0;
}

/* --------------------------------------------------------------- a site --- */

/* Whether a site w x d with its back's middle at B, facing f, stands on `want`
 * ground clear of everything, with `walk` clear in front of it; fills sp if so. */
static int candidate(SGen *S, DVec2 B, DVec2 f, double w, double d, double walk, int want, VendorSpot *sp) {
    DVec2 r = dv(f.z, -f.x);
    OBB box = site_box(B, f, w, d), way = way_box(B, f, w, d, walk);
    for (int i = 0; i < S->n; ++i)
        if (obb_hit(&box, &S->box[i], V_GAP_MIN) || obb_hit(&box, &S->way[i], 0.02) || obb_hit(&way, &S->box[i], 0.02)) return 0;
    for (int i = 0; i < S->nkeep; ++i)
        if (obb_hit(&box, &S->keep[i], 0.0)) return 0;
    int kerb = want == GK_SIDEWALK;
    int nu = (int)ceil(w / SAMPLE), nz = (int)ceil(d / SAMPLE);
    float roof = 1e30f;   /* the lowest overhead, world Y */
    for (int j = 0; j <= nz; ++j)
        for (int i = 0; i <= nu; ++i) {
            double a = -w * 0.5 + w * i / nu, b = 0.02 + (d - 0.04) * j / nz;
            DVec2 p = dadd(B, dadd(dscale(r, a), dscale(f, b)));
            if (ground_at(S, p.x, p.z) != want || solid(S, p.x, p.z, 0.02f) || in_doorway(S, p.x, p.z)) return 0;
            if (kerb) {
                if (lamp_near(S, p.x, p.z, KERB_LAMP)) return 0;
                for (int q = 0; q < S->nd; ++q)
                    if (city_drop_contains(&S->dk[q], p.x, p.z, KERB_CROSSING)) return 0;
            }
            if ((i == 0 || i == nu || i == nu / 2) && (j == 0 || j == nz || j == nz / 2)) {
                float h = overhead(S, p.x, p.z);
                if (h - city_ground_y(p.x, p.z) - SIDEWALK_HEIGHT < CEIL_MIN) return 0;
                roof = minf(roof, h);
            }
        }
    /* the way in front: on the block's ground, nothing solid in it */
    int nw = (int)ceil(walk / SAMPLE);
    for (int j = 1; j <= nw; ++j)
        for (int i = 0; i <= nu; ++i) {
            double a = -w * 0.5 + w * i / nu, b = d + walk * j / nw;
            DVec2 p = dadd(B, dadd(dscale(r, a), dscale(f, b)));
            if (ground_at(S, p.x, p.z) == GK_NONE || solid(S, p.x, p.z, 0.0f) || (kerb && lamp_near(S, p.x, p.z, 0.0))) return 0;
        }
    /* a wall all along its back, which a lean-to may lean on */
    int host = 1;
    for (int i = 0; i <= nu && host; ++i) {
        DVec2 p = dadd(B, dadd(dscale(r, -w * 0.5 + w * (0.05 + 0.9 * i / nu)), dscale(f, -0.12)));
        host = solid(S, p.x, p.z, 0.0f);
    }
    memset(sp, 0, sizeof(*sp));
    sp->x = B.x; sp->z = B.z;
    sp->fx = (float)f.x; sp->fz = (float)f.z;
    float lo = 1e30f;
    double cv[4][2];
    sp->site.w = (float)w; sp->site.d = (float)d;
    vendor_spot_corners(sp, cv);
    for (int k = 0; k < 4; ++k) lo = minf(lo, city_ground_y(cv[k][0], cv[k][1]));
    sp->y = lo + SIDEWALK_HEIGHT + (want == GK_LOT ? 0.006f : 0.0f);
    double mx, mz;
    vendor_spot_middle(sp, &mx, &mz);
    sp->ground = city_ground_y(mx, mz);
    sp->walk = (float)walk;
    sp->site.ceiling = roof - sp->y >= OPEN_SKY ? 0.0f : roof - sp->y;
    sp->site.affluence = plan_affluence(mx, mz);
    sp->site.hostWall = (uint8_t)host;
    sp->site.archetype = -1; sp->site.trade = -1;
    return 1;
}

static int g_bars;
void city_force_bars(int on) { g_bars = on; }
int  city_forced_bars(void) { return g_bars; }

static void keep_site(SGen *S, const VendorSpot *sp, int space, uint32_t group, int member, int lunch) {
    if (S->n >= MAX_BLOCK_SPOTS) return;
    VendorSpot *o = &S->spot[S->n];
    *o = *sp;
    o->space = (uint8_t)space;
    o->group = group; o->member = (uint16_t)member; o->members = 1;
    o->site.kind = (uint8_t)site_kind(space);
    o->site.lunch = (uint8_t)lunch;
    o->site.seed = hash_combine(group, (uint32_t)member + 1u);
    if (space == SPACE_APRON && o->site.hostWall && o->site.ceiling == 0.0f && vendor_spot_contains(o, VENDOR_SLICE_X, VENDOR_SLICE_Z, 0.0, 0.0)) {
        o->site.slice = 1;
        o->site.archetype = VA_CANOPY;
        o->site.trade = VT_NOODLE;
    }
    else if (g_bars && space <= SPACE_KERB) {
        float mw, md;
        vendor_min_site(VA_CANOPY, &mw, &md);
        if (o->site.w >= mw && o->site.d >= md && (o->site.ceiling == 0.0f || o->site.ceiling >= 2.9f)) {
            o->site.archetype = VA_CANOPY;   /* its trade chosen as any bar's (SF1) */
        }
    }
    DVec2 B = dv(sp->x, sp->z), f = dv(sp->fx, sp->fz);
    S->box[S->n] = site_box(B, f, sp->site.w, sp->site.d);
    S->way[S->n] = way_box(B, f, sp->site.w, sp->site.d, sp->walk);
    S->n++;
}

/* ---------------------------------------------------------------- rows --- */

typedef struct Row {
    DVec2    a, t, f;        /* the back line's start, along it, the sites' front */
    double   len;
    float    dlo, dhi, wlo, whi, walk;
    int      want, space, lunch, most;
    float    rate;           /* kerb pitches: how many to a metre (exponential gaps); 0: a packed row */
    uint32_t seed, group;    /* group 0: each site a group of its own */
} Row;

static void fill_row(SGen *S, const Row *R, int *member) {
    if (R->len < R->wlo || R->dhi < R->dlo - 1e-3f) return;
    Rng r = rng_make(R->seed);
    double u = R->rate > 0.0f ? -log(1.0 - 0.999 * rng_float(&r)) / R->rate : rng_range(&r, 0.0f, 1.0f);
    int placed = 0, tries = 0;
    while (u < R->len && placed < R->most && S->n < MAX_BLOCK_SPOTS) {
        double w = rng_range(&r, R->wlo, R->whi), d = rng_range(&r, R->dlo, R->dhi);
        if (u + w > R->len) w = R->len - u;
        if (w < R->wlo - 1e-6) break;
        VendorSpot sp;
        int ok = 0;
        for (int k = 0; k < 2 && !ok; ++k) {
            double ww = k ? R->wlo : w;
            if (k && w - R->wlo < 0.3) break;
            DVec2 B = dadd(R->a, dscale(R->t, u + ww * 0.5));
            if (candidate(S, B, R->f, ww, d, R->walk, R->want, &sp)) { ok = 1; w = ww; }
        }
        if (ok) {
            uint32_t grp = R->group ? R->group : hash_combine(R->seed, (uint32_t)placed + 1u);
            keep_site(S, &sp, R->space, grp, R->group ? (*member)++ : 0, R->lunch);
            placed++;
            tries = 0;
            if (R->rate > 0.0f) u += w + (rng_float(&r) < 0.3f ? rng_range(&r, 1.0f, 2.0f) : -log(1.0 - 0.999 * rng_float(&r)) / R->rate);
            else u += w + rng_range(&r, (float)V_GAP_MIN + 0.05f, 2.2f);
        } else if (R->rate > 0.0f && ++tries > 4) {
            u += -log(1.0 - 0.999 * rng_float(&r)) / R->rate;
            tries = 0;
        } else {
            u += 0.5;
        }
    }
}

/* A row along a space's frame: from (u, v) along unit direction (du, dv) in
 * the frame for len, the sites facing (fu, fv). */
static Row space_row(const CitySpace *s, double u, double v, double du, double dvv, double len, double fu, double fv) {
    Row R;
    memset(&R, 0, sizeof(R));
    DVec2 U = dv(s->ux, s->uz), V = dv(s->vx, s->vz);
    R.a = dadd(dv(s->ox, s->oz), dadd(dscale(U, u), dscale(V, v)));
    R.t = dadd(dscale(U, du), dscale(V, dvv));
    R.f = dadd(dscale(U, fu), dscale(V, fv));
    R.len = len;
    R.want = GK_LOT;
    R.walk = VENDOR_WALK_LOT;
    R.group = s->seed;
    R.space = s->kind;
    return R;
}

/* ------------------------------------------------------------ lane vaults --- */

static int free_at(const float *a, const float *b, int n, float x) {
    for (int i = 0; i < n; ++i) if (x > a[i] - 0.3f && x < b[i] + 0.3f) return 0;
    return 1;
}

/* SF2: a vault over a market lane (src/gen/vault.h), where the user said it makes sense: a lane between buildings,
 * both its sides mostly walled by the block's buildings. Its posts stand under its girders in the gaps between
 * the stalls of each row (and at its ends); the stalls under it get the girders' underside as their ceiling. */
static int walled(const SGen *S, DVec2 a, DVec2 t, DVec2 out, double len) {
    int n = 0, hit = 0;
    for (double u = 1.0; u < len - 1.0; u += 3.0, ++n) {
        DVec2 p = dadd(dadd(a, dscale(t, u)), dscale(out, 1.2));
        int in = 0;
        for (int k = 0; k < S->nbl && !in; ++k) in = inside_building(&S->g->bld[S->bl[k]], p.x, p.z, 0.0);
        hit += in;
    }
    return n > 0 && (double)hit >= 0.6 * (double)n;
}

/* Whether a vault's post at (x, z) of its frame keeps clear of the sites planned before its lane's (and their ways). */
static int post_clear(const SGen *S, const CityVault *cv, int first, float x, float z) {
    OBB o;
    vault_world(cv, x, z, &o.c.x, &o.c.z);
    o.t = dv(1.0, 0.0); o.n = dv(0.0, 1.0); o.hu = o.hn = 0.17;
    for (int k = 0; k < first; ++k)
        if (obb_hit(&o, &S->box[k], 0.2) || obb_hit(&o, &S->way[k], 0.05)) return 0;
    return 1;
}

static void lane_vault(SGen *S, const CitySpace *s, int first) {
    if (S->nvault >= MAX_BLOCK_VAULTS) return;
    const uint32_t h = hash_combine(s->seed, 0xFA17u);
    if (hash_float(h) >= 0.65f) return;
    const DVec2 U = dv(s->ux, s->uz), V = dv(s->vx, s->vz), O = dv(s->ox, s->oz);
    const double ua = s->u0 + 0.3, ub = s->u1 - 0.3, len = ub - ua, span = s->v1 - s->v0;
    if (len < 8.0 || span < 5.0 || span > 12.0) return;
    /* both sides walled */
    const DVec2 a0 = dadd(O, dadd(dscale(U, ua), dscale(V, s->v0))), a1 = dadd(O, dadd(dscale(U, ua), dscale(V, s->v1)));
    if (!walled(S, a0, U, dscale(V, -1.0), len) || !walled(S, a1, U, V, len)) return;
    CityVault *cv = &S->vault[S->nvault];
    memset(cv, 0, sizeof(*cv));
    /* the frame: its +z across (V), its +x (V.z, -V.x) along the lane, from the lane's start that way */
    const DVec2 xd = dv(V.z, -V.x);
    const double us = ddot(xd, U) > 0.0 ? ua : ub, ud = ddot(xd, U) > 0.0 ? 1.0 : -1.0;
    cv->seed = h;
    cv->affluence = S->aff;
    VaultPlan *P = &cv->plan;
    vault_plan(P, (float)len, (float)span, h);
    /* its floor: the lane's lowest, paved */
    float lo = 1e30f;
    for (double u = 0.0; u <= len + 1e-6; u += 2.0)
        for (int e = 0; e < 3; ++e) {
            const DVec2 q = dadd(O, dadd(dscale(U, us + ud * u), dscale(V, s->v0 + span * 0.5 * e)));
            lo = minf(lo, city_ground_y(q.x, q.z));
        }
    const float floorY = lo + SIDEWALK_HEIGHT + 0.006f;
    /* the user (SF2): attached to the buildings' sides at their first floor. Each side's wall: how far out of the
     * lane's edge a building's wall stands at each rib (within 3 m), and that building's first floor */
    float wd[2][VAULT_BAYS_MAX], dmin[2] = { 99.0f, 99.0f }, floor1 = 99.0f;
    for (int side = 0; side < 2; ++side)
        for (int b = 0; b < P->nbay; ++b) {
            const DVec2 out = side ? V : dscale(V, -1.0);
            const DVec2 e = dadd(O, dadd(dscale(U, us + ud * (P->pitch * b + (b == 0 ? 0.3 : 0.0))), dscale(V, side ? s->v1 : s->v0)));
            wd[side][b] = 99.0f;
            for (double t = 0.0; t <= 3.0; t += 0.1) {
                const DVec2 q = dadd(e, dscale(out, t));
                if (!solid(S, q.x, q.z, 0.02f)) continue;
                wd[side][b] = (float)t;
                for (int k = 0; k < S->nbl; ++k) {
                    float lx, lz;
                    const Building *bd = &S->g->bld[S->bl[k]];
                    if (in_plot(bd, q.x, q.z, 0.2, &lx, &lz))
                        floor1 = minf(floor1, building_base(bd) + chunk_gen_graph(S->g, S->bl[k])->groundH - floorY);
                }
                break;
            }
            dmin[side] = minf(dmin[side], wd[side][b]);
        }
    if (dmin[0] > 3.0f || dmin[1] > 3.0f || floor1 > 50.0f) return;
    /* the span wall face to wall face (the nearest faces), the springing at the first floor */
    vault_plan(P, (float)len, (float)span + dmin[0] + dmin[1], h);
    vault_set_spring(P, clampf(floor1, 3.6f, 6.5f));
    for (int side = 0; side < 2; ++side)
        for (int b = 0; b < P->nbay; ++b) P->wall[side][b] = (uint8_t)(wd[side][b] <= dmin[side] + 0.25f);
    const DVec2 org = dadd(O, dadd(dscale(U, us), dscale(V, s->v0 - dmin[0])));
    cv->x = org.x; cv->z = org.z; cv->fx = (float)V.x; cv->fz = (float)V.z;
    cv->y = floorY;
    /* the stalls of each side, along the vault's x */
    float sx0[2][MAX_BLOCK_SPOTS], sx1[2][MAX_BLOCK_SPOTS];
    int ns[2] = { 0, 0 };
    for (int k = first; k < S->n; ++k) {
        VendorSpot *sp = &S->spot[k];
        const int side = sp->fx * V.x + sp->fz * V.z > 0.0 ? 0 : 1;
        const double lx = (sp->x - cv->x) * xd.x + (sp->z - cv->z) * xd.z;
        sx0[side][ns[side]] = (float)lx - 0.5f * sp->site.w; sx1[side][ns[side]] = (float)lx + 0.5f * sp->site.w;
        ns[side]++;
        /* under the girders: the vault is its ceiling */
        const float c = cv->y + vault_clear(P) - sp->y;
        if (sp->site.ceiling == 0.0f || sp->site.ceiling > c) sp->site.ceiling = c;
        sp->site.vaulted = 1;
    }
    /* posts: from either end, each as far on as it can stand within 6 m, clear of the stalls by 0.2 m (SF3: and of
     * the sites planned before the lane's, the shop bays' and corners' reaching into it, and their ways) */
    for (int side = 0; side < 2; ++side) {
        const float pz = side ? P->span - P->inset : P->inset;
        #define FREE(x) (free_at(sx0[side], sx1[side], ns[side], (x)) && post_clear(S, cv, first, (x), pz))
        float cur = -1.0f;
        for (float x = 0.25f; x < 3.0f; x += 0.1f) if (FREE(x)) { cur = x; break; }
        if (cur < 0.0f) cur = 0.25f;
        vault_add_post(P, side, cur);
        while (cur < P->len - 0.3f) {
            float next = -1.0f;
            for (float x = minf(cur + 6.0f, P->len - 0.25f); x > cur + 1.0f; x -= 0.1f) if (FREE(x)) { next = x; break; }
            if (next < 0.0f) for (float x = cur + 6.0f; x < P->len - 0.25f; x += 0.1f) if (FREE(x)) { next = x; break; }
            if (next < 0.0f) break;
            if (!vault_add_post(P, side, next)) break;
            cur = next;
        }
        #undef FREE
    }
    S->nvault++;
}

/* The rows of one of the filler's spaces. */
static void fill_space(SGen *S, const CitySpace *s) {
    int member = 0;
    double W = s->u1 - s->u0, H = s->v1 - s->v0;
    uint32_t sd = s->seed;
    Row R;
    switch (s->kind) {
    case SPACE_APRON: {
        /* backs to the row's front, facing the street; customers stand on the apron and the sidewalk. SF3: only
         * some aprons, a few each (the slice's whole: its site is the noodle bar's) */
        const int slice = city_space_contains(s, VENDOR_SLICE_X, VENDOR_SLICE_Z, 0.0);
        if (!slice && hash_float(hash_combine(sd, 0xA9u)) >= APRON_SHARE) break;
        R = space_row(s, s->u0 + 0.4, s->v1 - 0.05, 1, 0, W - 0.8, 0, -1);
        R.dlo = 2.4f; R.dhi = (float)fmin(3.6, H - 1.0); if (R.dhi < R.dlo) R.dlo = R.dhi;
        R.wlo = 2.4f; R.whi = 4.5f; R.walk = 1.0f; R.most = slice ? 12 : APRON_MOST; R.seed = hash_combine(sd, 1u);
        fill_row(S, &R, &member);
        break;
    }
    case SPACE_LANE: {
        /* a row down each side, facing across the way between them */
        const int first = S->n;
        double d = fmin(3.5, (H - VENDOR_WALK_LOT - 0.2) * 0.5);
        for (int side = 0; side < 2; ++side) {
            R = side ? space_row(s, s->u1 - 1.0, s->v1 - 0.05, -1, 0, W - 2.0, 0, -1)
                     : space_row(s, s->u0 + 1.0, s->v0 + 0.05, 1, 0, W - 2.0, 0, 1);
            R.dlo = (float)fmin(2.4, d); R.dhi = (float)d;
            R.wlo = 2.2f; R.whi = 4.0f; R.walk = (float)fmin(VENDOR_WALK_LOT, H - 2.0 * d - 0.2); R.most = 16;
            R.seed = hash_combine(sd, 2u + (uint32_t)side);
            fill_row(S, &R, &member);
        }
        lane_vault(S, s, first);   /* SF2 */
        break;
    }
    case SPACE_POCKET: {
        /* rows across the plot, in from the street, facing each other across aisles open to it */
        const double d = 2.5, aisle = 2.2 + 0.8 * hash_float(hash_combine(sd, 9u)), unit = 2.0 * d + aisle + V_GAP_MIN + 0.1;
        int na = (int)floor((W + V_GAP_MIN + 0.1) / unit);
        if (na < 1) break;
        double u = s->u0 + (W - (na * unit - V_GAP_MIN - 0.1)) * 0.5;
        for (int k = 0; k < na; ++k, u += unit)
            for (int side = 0; side < 2; ++side) {
                double ub = side ? u + 2.0 * d + aisle : u;
                R = space_row(s, ub, s->v0 + 1.2, 0, 1, H - 1.7, side ? -1 : 1, 0);
                R.dlo = R.dhi = (float)d;
                R.wlo = 2.2f; R.whi = 3.6f; R.most = 8;
                R.seed = hash_combine(sd, 10u + (uint32_t)(2 * k + side));
                fill_row(S, &R, &member);
            }
        break;
    }
    case SPACE_PLAZA:
        /* a few lunch stalls at the forecourt's sides, the middle (the way to the tower) clear */
        for (int side = 0; side < 2; ++side) {
            R = space_row(s, side ? s->u1 - 0.5 : s->u0 + 0.5, s->v0 + 1.5, 0, 1, H - 2.5, side ? -1 : 1, 0);
            R.dlo = 2.5f; R.dhi = (float)fmin(4.0, W * 0.25);
            R.wlo = 2.4f; R.whi = 7.0f; R.most = 2; R.lunch = 1;
            R.seed = hash_combine(sd, 20u + (uint32_t)side);
            fill_row(S, &R, &member);
        }
        break;
    case SPACE_COURT: {
        /* round its sides, facing in, the middle clear */
        double dm = fmin(6.0, (fmin(W, H) - 3.0) * 0.5);
        const double cu[4] = { s->u0 + 0.05, s->u1 - 0.05, s->u1 - 0.05, s->u0 + 0.05 }, cv[4] = { s->v0 + 0.05, s->v0 + 0.05, s->v1 - 0.05, s->v1 - 0.05 };
        const double tu[4] = { 1, 0, -1, 0 }, tv[4] = { 0, 1, 0, -1 }, len[4] = { W - 0.1, H - 0.1, W - 0.1, H - 0.1 };
        int first = (int)(hash_combine(sd, 30u) & 3u), total = 0;
        for (int q = 0; q < 4; ++q) {
            int e = (first + q) & 3;
            R = space_row(s, cu[e], cv[e], tu[e], tv[e], len[e], -tv[e], tu[e]);   /* in: the edge turned left */
            R.dlo = 2.4f; R.dhi = (float)dm;
            R.wlo = 2.5f; R.whi = 7.0f; R.most = 8 - total;
            R.seed = hash_combine(sd, 31u + (uint32_t)e);
            int before = S->n;
            fill_row(S, &R, &member);
            total += S->n - before;
        }
        break;
    }
    default: break;
    }
}

/* ---------------------------------------------------------------- courts --- */

/* The yard behind the block's buildings: the largest rectangle (COURT_MIN
 * square at least) of open lot no plot nor space takes, along the grain, if a
 * way at least VENDOR_WALK_LOT wide leads from it to the street. Its ways in
 * are kept clear (S->keep). */
static int find_court(SGen *S, CitySpace *out) {
    const Block *bk = S->bk;
    if (!bk->lot.n || (bk->zone != ZONE_CORE && bk->zone != ZONE_CENTRE)) return 0;
    if (hash_float(hash_combine(bk->seed, 0xC0027u)) >= (S->aff < 0.66f ? 0.35f : 0.15f)) return 0;
    DVec2 t = dv(bk->gc, bk->gs), nn = dv(-bk->gs, bk->gc);
    double u0 = 1e300, u1 = -1e300, v0 = 1e300, v1 = -1e300;
    for (int i = 0; i < bk->curb.n; ++i) {
        double a = ddot(bk->curb.v[i], t), b = ddot(bk->curb.v[i], nn);
        u0 = fmin(u0, a); u1 = fmax(u1, a); v0 = fmin(v0, b); v1 = fmax(v1, b);
    }
    const double h = COURT_CELL;
    int NU = (int)ceil((u1 - u0) / h), NV = (int)ceil((v1 - v0) / h);
    if (NU < 8 || NV < 8 || (long)NU * NV > 600000) return 0;
    /* per cell: 1 open lot (a court may stand there), 2 walkable, 4 the street's sidewalk */
    uint8_t *c = (uint8_t *)calloc((size_t)NU * NV, 1);
    int *q = (int *)malloc(sizeof(int) * (size_t)NU * NV);
    if (!c || !q) fatal("out of memory finding a court");
    for (int j = 0; j < NV; ++j)
        for (int i = 0; i < NU; ++i) {
            double a = u0 + (i + 0.5) * h, b = v0 + (j + 0.5) * h;
            double x = t.x * a + nn.x * b, z = t.z * a + nn.z * b;
            if (!poly_contains(&bk->curb, x, z, 0.0)) continue;
            int built = 0;
            for (int k = 0; k < S->nbl && !built; ++k) built = inside_building(&S->g->bld[S->bl[k]], x, z, 0.6);
            if (built) continue;
            uint8_t m = 2;
            if (!poly_contains(&bk->lot, x, z, 0.0)) m |= 4;
            else if (poly_contains(&bk->lot, x, z, -0.3)) {
                int sp = 0;
                for (int k = 0; k < S->nsp && !sp; ++k) sp = city_space_contains(&S->sp[k], x, z, 0.5);
                if (!sp) m |= 1;
            }
            c[(size_t)j * NU + i] = m;
        }
    /* the largest open rectangle at least COURT_MIN square (histograms row by row) */
    int need = (int)ceil(COURT_MIN / h), best = 0, bi0 = 0, bj0 = 0, bi1 = 0, bj1 = 0;
    int *hist = (int *)calloc((size_t)NU + 1, sizeof(int)), *stk = (int *)malloc(sizeof(int) * ((size_t)NU + 1));
    if (!hist || !stk) fatal("out of memory finding a court");
    for (int j = 0; j < NV; ++j) {
        for (int i = 0; i < NU; ++i) hist[i] = (c[(size_t)j * NU + i] & 1) ? hist[i] + 1 : 0;
        hist[NU] = 0;
        int sp = 0;
        for (int i = 0; i <= NU; ++i) {
            while (sp && hist[stk[sp - 1]] >= hist[i]) {
                int hh = hist[stk[--sp]], left = sp ? stk[sp - 1] + 1 : 0, ww = i - left;
                if (hh >= need && ww >= need && hh * ww > best) { best = hh * ww; bi0 = left; bi1 = i; bj0 = j - hh + 1; bj1 = j + 1; }
            }
            stk[sp++] = i;
        }
    }
    free(hist); free(stk);
    int ok = 0;
    if (best) {
        /* way: a cell walkable with its four neighbours (a way of three cells, 1.5 m, at least) */
        #define WALK(i, j) ((i) > 0 && (j) > 0 && (i) < NU - 1 && (j) < NV - 1 && (c[(size_t)(j) * NU + (i)] & 2) && \
                            (c[(size_t)(j) * NU + (i) - 1] & 2) && (c[(size_t)(j) * NU + (i) + 1] & 2) && \
                            (c[(size_t)((j) - 1) * NU + (i)] & 2) && (c[(size_t)((j) + 1) * NU + (i)] & 2))
        uint8_t *seen = (uint8_t *)calloc((size_t)NU * NV, 1);
        if (!seen) fatal("out of memory finding a court");
        int qh = 0, qt = 0, street = 0;
        int ci = (bi0 + bi1) / 2, cj = (bj0 + bj1) / 2;
        if (WALK(ci, cj)) { seen[(size_t)cj * NU + ci] = 1; q[qt++] = cj * NU + ci; }
        while (qh < qt && !street) {
            int at = q[qh++], i = at % NU, j = at / NU;
            if (c[at] & 4) street = 1;
            static const int di[4] = { 1, -1, 0, 0 }, dj[4] = { 0, 0, 1, -1 };
            for (int k = 0; k < 4; ++k) {
                int a = i + di[k], b = j + dj[k];
                if (!WALK(a, b) || seen[(size_t)b * NU + a]) continue;
                seen[(size_t)b * NU + a] = 1;
                q[qt++] = b * NU + a;
            }
        }
        if (street) {
            /* the court, a little in from the cells round it; its ways in: the walkable cells just outside each side */
            memset(out, 0, sizeof(*out));
            out->kind = SPACE_COURT;
            out->ox = 0.0; out->oz = 0.0;
            out->ux = (float)t.x; out->uz = (float)t.z; out->vx = (float)nn.x; out->vz = (float)nn.z;
            out->u0 = (float)(u0 + bi0 * h + 0.25); out->u1 = (float)(u0 + bi1 * h - 0.25);
            out->v0 = (float)(v0 + bj0 * h + 0.25); out->v1 = (float)(v0 + bj1 * h - 0.25);
            out->seed = hash_combine(bk->seed ^ 0xC0027u, 1u);
            double dm = fmin(6.0, (fmin(out->u1 - out->u0, out->v1 - out->v0) - 3.0) * 0.5) + 0.5;
            for (int side = 0; side < 4; ++side) {
                int horiz = side < 2, n = horiz ? bi1 - bi0 : bj1 - bj0, run = -1;
                for (int k = 0; k <= n; ++k) {
                    int i, j;
                    if (horiz) { i = bi0 + k; j = side == 0 ? bj0 - 1 : bj1; }
                    else       { j = bj0 + k; i = side == 2 ? bi0 - 1 : bi1; }
                    int open = k < n && i >= 0 && j >= 0 && i < NU && j < NV && seen[(size_t)j * NU + i];
                    if (open && run < 0) run = k;
                    if (!open && run >= 0) {
                        if (k - run >= 3 && S->nkeep < MAX_KEEP) {
                            /* the way in across the side's run, from the side to past the rows */
                            double a0 = (horiz ? u0 + (bi0 + run) * h : v0 + (bj0 + run) * h) - 0.3, a1 = (horiz ? u0 + (bi0 + k) * h : v0 + (bj0 + k) * h) + 0.3;
                            double e = horiz ? (side == 0 ? out->v0 : out->v1) : (side == 2 ? out->u0 : out->u1);
                            double in = (side == 0 || side == 2) ? 1.0 : -1.0, mid = e + in * dm * 0.5;
                            OBB *o = &S->keep[S->nkeep++];
                            double cu = horiz ? 0.5 * (a0 + a1) : mid, cv = horiz ? mid : 0.5 * (a0 + a1);
                            o->c = dadd(dscale(t, cu), dscale(nn, cv));
                            o->t = t; o->n = nn;
                            o->hu = horiz ? 0.5 * (a1 - a0) : dm * 0.5; o->hn = horiz ? dm * 0.5 : 0.5 * (a1 - a0);
                        }
                        run = -1;
                    }
                }
            }
            ok = 1;
        }
        free(seen);
        #undef WALK
    }
    free(c); free(q);
    return ok;
}

/* ----------------------------------------------------------------- kerbs --- */

/* Pitches by the kerb along edge i of the block, if its sidewalk is wide
 * enough to leave VENDOR_WALK_KERB behind them: barrows and kiosks, most in
 * the poorer centres, lunch kiosks in the wealthy cores. */
static void kerb_pitches(SGen *S, int i) {
    const Block *bk = S->bk;
    const Poly *c = &bk->curb;
    if (!c->e[i].id || c->e[i].bank) return;
    DVec2 A = c->v[i], B = c->v[(i + 1) % c->n];
    double len = dlen(dsub(B, A)), walk = c->e[i].walk;
    double room = walk - KERB_BACK - VENDOR_WALK_KERB - 0.05;
    if (len < 2.0 * KERB_END + 2.0 || room < 1.7) return;
    static const float RATE[ZONE_COUNT] = { 1.0f / 160.0f, 1.0f / 55.0f, 1.0f / 140.0f, 1.0f / 260.0f, 0.0f, 0.0f };
    float rate = RATE[bk->zone] * (1.4f - S->aff) * KERB_SHARE;
    if (rate <= 0.0f) return;
    DVec2 t = dscale(dsub(B, A), 1.0 / len), in = dv(-t.z, t.x);
    if (ddot(in, dsub(poly_centroid(c), A)) < 0.0) in = dv(-in.x, -in.z);
    Row R;
    memset(&R, 0, sizeof(R));
    R.a = dadd(dadd(A, dscale(t, KERB_END)), dscale(in, KERB_BACK));
    R.t = t; R.f = in;
    R.len = len - 2.0 * KERB_END;
    R.dlo = 1.7f; R.dhi = (float)fmin(2.6, room);
    R.wlo = 2.0f; R.whi = 3.0f; R.walk = VENDOR_WALK_KERB;
    R.want = GK_SIDEWALK; R.space = SPACE_KERB;
    R.lunch = bk->zone == ZONE_CORE && S->aff > 0.6f;
    R.most = 64; R.rate = rate;
    R.seed = hash_combine(bk->seed ^ 0x4E4Bu, (uint32_t)i);
    int member = 0;
    fill_row(S, &R, &member);
}

/* ------------------------------------------------------------ shop bays --- */

#define SHOP_BAY      6.0f    /* storefront bays (city_shop_hash, shadeFacade) */
#define SHOP_OPEN     5.4f    /* the opening: the bay's shop window, its 0.3 m piers kept */
#define SHOP_SITE     5.5f    /* the site: the opening and 5 cm each side (the hood's end plates) */
#define SHOP_OUT      1.45    /* the stall outside the building line with stools... */
#define SHOP_OUT_MIN  0.75    /* ...and with a standing counter */
#define DEEP_MIN      5.0f    /* the deep unit's room: as deep as the shops' interior mapping (5 m), give or take */
#define DEEP_SPAN     0.7f

/* Whether a point lies in any site planned so far (but site skip). */
static int in_site(const SGen *S, DVec2 p, int skip) {
    for (int o = 0; o < S->n; ++o) {
        if (o == skip) continue;
        const OBB *b = &S->box[o];
        const DVec2 d = dsub(p, b->c);
        if (fabs(ddot(d, b->t)) <= b->hu && fabs(ddot(d, b->n)) <= b->hn) return 1;
    }
    return 0;
}

/* On a lot, from q in front of a building's face (along t, out f): not a yard shut in behind the buildings, nor an
 * alley too narrow to walk out of or stopped by the stalls planned in it: a way out of it, a walker's width (0.35 m
 * clear each side of walls, columns and sites), reaches a street's sidewalk or open ground, or (not street: a deep
 * unit's door wants the street) runs on 20 m (the street within 30 m). */
static int lot_open(SGen *S, DVec2 q, DVec2 t, DVec2 f, int skip, int street) {
    const DVec2 dir[5] = { f, dadd(f, dscale(t, 0.7)), dsub(f, dscale(t, 0.7)), t, dscale(t, -1.0) };
    const double reach = street ? 30.0 : 20.0;
    for (int r = 0; r < 5; ++r) {
        const DVec2 d = dscale(dir[r], 1.0 / dlen(dir[r])), n = dv(-d.z, d.x);
        for (double l = 0.0; l <= reach; l += 0.25) {
            const DVec2 p = dadd(q, dscale(d, l));
            const int gk = ground_at(S, p.x, p.z);
            if (gk == GK_SIDEWALK || gk == GK_NONE || gk == GK_DROP || (!street && l >= reach)) return 1;
            const DVec2 pa = dadd(p, dscale(n, 0.35)), pb = dsub(p, dscale(n, 0.35));
            if (solid(S, p.x, p.z, 0.05f) || solid(S, pa.x, pa.z, 0.0f) || solid(S, pb.x, pb.z, 0.0f) ||
                in_site(S, p, skip) || in_site(S, pa, skip) || in_site(S, pb, skip)) break;
        }
    }
    return 0;
}

/* Whether the outside of a shop bay's stall is clear: from the building line at P (along t, out f), w across and out
 * deep, on the block's sidewalk or lot ground, clear of walls, columns, doors' ways, lamps and crossings, under the
 * open sky or high enough a ceiling (an arcade's), with the walk beyond it (walkOut) clear as well. */
static int shop_clear(SGen *S, DVec2 P, DVec2 t, DVec2 f, double w, double out, float floorY, float head, double want, int street, double *walkOut) {
    const int kind = ground_at(S, P.x + f.x * 0.3, P.z + f.z * 0.3);
    if (kind != GK_SIDEWALK && (kind != GK_LOT || want > 0.0)) return 0;   /* (a narrower walk asked only off a street's sidewalk) */
    if (kind == GK_LOT && !lot_open(S, dadd(P, dscale(f, out + 0.3)), t, f, -1, street)) return 0;
    /* under an arcade the walk is the colonnade's, between its columns */
    const int arcade = overhead(S, P.x + f.x * 0.3, P.z + f.z * 0.3) < 1e29f;
    const double walk = want > 0.0 ? want : arcade ? 1.2 : kind == GK_SIDEWALK ? VENDOR_WALK_KERB : VENDOR_WALK_LOT;
    const int nu = (int)ceil(w / SAMPLE), nz = (int)ceil(out / SAMPLE), nw = (int)ceil(walk / SAMPLE);
    for (int j = 0; j <= nz + nw; ++j)
        for (int i = 0; i <= nu; ++i) {
            const int way = j > nz;
            const double a = -0.5 * w + w * i / nu, b = way ? out + walk * (j - nz) / nw : 0.05 + (out - 0.05) * j / nz;
            const DVec2 p = dadd(P, dadd(dscale(t, a), dscale(f, b)));
            const int gk = ground_at(S, p.x, p.z);
            if (gk != GK_SIDEWALK && gk != GK_LOT) return 0;
            if (solid_ex(S, p.x, p.z, way ? 0.0f : 0.02f, !(way && arcade && b > out + 0.45))) return 0;
            if (lamp_near(S, p.x, p.z, way ? 0.0 : KERB_LAMP)) return 0;
            if (way) continue;
            for (int q = 0; q < S->nd; ++q)
                if (city_drop_contains(&S->dk[q], p.x, p.z, KERB_CROSSING)) return 0;
            if ((i == 0 || i == nu) && j == nz && overhead(S, p.x, p.z) - floorY < head + 0.1f) return 0;
        }
    *walkOut = arcade ? 0.4 : walk;   /* (the colonnade's checked here; the site keeps its customers' room clear) */
    return 1;
}

/* SF3: the stalls taking over lit shop bays (the user's choice: always taking the bay over), about one bay in 15:
 * one in 8 where the district is poorest, one in 30 where richest, three times likelier beside a stall. Each whole
 * bay that isn't a corner shop; its floor at the sidewalk's; the recess inside its section; the outside clear.
 * On the first board the user asked for deep units as well (the bar at the back of a room behind the bay's own
 * shopfront, about as deep as the shops' interior mapping): as many again, half the bays chosen each. A deep unit
 * needs only its doorstep outside (the walk is the sidewalk's own); an awning over the sidewalk where it's clear for
 * one and not under an arcade; an A-frame where there's 0.6 m. */
static void shop_stalls(SGen *S) {
    ChunkGen *g = S->g;
    for (int q = 0; q < S->nbl; ++q) {
        const int i = S->bl[q];
        const Building *b = &g->bld[i];
        if (!b->shop) continue;
        const BGraph *bg = chunk_gen_graph(g, i);
        if (bgen_industrial(bg) || bg->groundMode != GF_RETAIL || bg->archetype == ARCH_ARCOLOGY) continue;
        const float base = building_base(b), headL = G0 + bg->groundH - 1.4f;   /* just over the shop window */
        /* each its own group, keyed by its bay: a stall is the same whichever chunk plans the block */
        const uint32_t grp = hash_combine(b->seed, 0x5A0Bu);
        for (int si = 0; si < bg->nsec; ++si) {
            const BSection *s = &bg->sec[si];
            if (!bsec_grounded(s) || !bsec_facade(s) || bsec_open(s) || relief_on(bg, s)) continue;
            const BLoop *l = &s->foot.outer;
            for (int k = 0; k < l->n; ++k) {
                const FaceKey fk = face_key(l, k, 0);
                const EdgeInfo e = edge_of(l, k);
                const int last = (int)floorf((e.len - 0.01f) / SHOP_BAY);
                int prev = 0;
                for (int seg = 0; (float)(seg + 1) * SHOP_BAY <= e.len + 1e-3f; ++seg) {
                    const int was = prev;
                    prev = 0;
                    if ((seg == 0 && fk.startConvex) || (seg >= last && fk.endConvex)) continue;   /* corner shops */
                    const uint32_t h = city_shop_hash(b->seed, si, fk.face, seg, e.len, fk.next, fk.endConvex);
                    if (!city_shop_lit(h, b->shop)) continue;
                    const float um = ((float)seg + 0.5f) * SHOP_BAY;
                    const float lx = e.ax + e.ex * um, lz = e.az + e.ez * um;
                    const DVec2 P = to_world(b, lx, lz);
                    const float aff = plan_affluence(P.x, P.z);
                    /* twice the user's rates: about half the bays chosen can't take a stall (the sidewalk too narrow,
                     * a lamp, a party wall, the door), so the bays that can make it up */
                    float chance = 2.0f * lerpf(1.0f / 8.0f, 1.0f / 30.0f, aff);
                    if (was) chance = minf(0.5f, chance * 3.0f);
                    const float pick = hash_float(hash_combine(h, 0x57A11u)), total = minf(0.75f, 2.0f * chance);
                    if (pick >= total) continue;
                    const int deep = pick >= 0.5f * total;
                    const DVec2 t = dv(b->c * e.ex - b->s * e.ez, b->s * e.ex + b->c * e.ez);
                    const DVec2 f = dv(b->c * -e.ez - b->s * e.ex, b->s * -e.ez + b->c * e.ex);   /* out of the shopfront */
                    /* the floor: the sidewalk's at the opening's ends, level, at the wall's foot or a little under it */
                    const DVec2 pa = dadd(P, dadd(dscale(t, -0.5 * SHOP_OPEN), dscale(f, 0.3)));
                    const DVec2 pb = dadd(P, dadd(dscale(t, 0.5 * SHOP_OPEN), dscale(f, 0.3)));
                    const float ga = city_ground_y(pa.x, pa.z), gb = city_ground_y(pb.x, pb.z);
                    const float floorY = minf(ga, gb) + SIDEWALK_HEIGHT, foot = base + s->y0;
                    if (fabsf(ga - gb) > 0.3f || floorY < foot - 0.6f) continue;
                    int door = 0;   /* the building's doors: none in the opening (their ways may pass beside the stall) */
                    for (int di = 0; di < bg->ndoor && !door; ++di)
                        door = bg->door[di].section == si && bg->door[di].face == k && fabsf(bg->door[di].u - um) < 0.5f * bg->door[di].width + 0.5f * SHOP_OPEN + 0.15f;
                    if (door) continue;
                    float head = base + headL - floorY;
                    if (head < 3.15f) continue;
                    head = minf(head, VENDOR_TOP_MAX - 0.02f);   /* (the cables keep over VENDOR_TOP_MAX) */
                    const float R = deep ? DEEP_MIN + DEEP_SPAN * hash_float(hash_combine(h, 0x5EC5u)) : 2.3f + 0.3f * hash_float(hash_combine(h, 0x5EC5u));
                    int inside = 1;   /* the recess (or the room) within the section */
                    for (int c = -1; c <= 1 && inside; ++c)
                        for (int r = deep ? 0 : 1; r < 2 && inside; ++r) {
                            const float a = 0.48f * SHOP_OPEN * (float)c, z = r ? R + 0.1f : 0.5f * R;
                            inside = bfoot_contains(&s->foot, lx + e.ex * a + e.ez * z, lz + e.ez * a - e.ex * z);
                        }
                    if (!inside) continue;
                    double out = SHOP_OUT, walk = 0.0;
                    if (deep) {
                        const int arcade = overhead(S, P.x + f.x * 0.3, P.z + f.z * 0.3) < 1e29f;
                        out = 1.0 + 0.25 * hash_float(hash_combine(h, 0xA7A2u));
                        if (arcade || hash_float(hash_combine(h, 0xA7A3u)) >= 0.55f || !shop_clear(S, P, t, f, SHOP_SITE, out, floorY, head, 0.0, deep, &walk)) {
                            out = 0.6;
                            if (!shop_clear(S, P, t, f, SHOP_SITE, out, floorY, head, 0.0, deep, &walk)) {
                                out = 0.3;   /* its doorstep, the sidewalk's own walk past it */
                                if (!shop_clear(S, P, t, f, SHOP_SITE, out, floorY, head, 1.2, deep, &walk)) continue;
                            }
                        }
                    } else if (!shop_clear(S, P, t, f, SHOP_SITE, out, floorY, head, 0.0, deep, &walk)) {
                        out = SHOP_OUT_MIN;
                        if (!shop_clear(S, P, t, f, SHOP_SITE, out, floorY, head, 0.0, deep, &walk)) continue;
                    }
                    const DVec2 B = dsub(P, dscale(f, R));
                    {   /* no column in the bay's outside (nor, a deep unit's, within 0.6 m of its doorstep: an arcade's
                         * column jammed against its shopfront) */
                        const OBB ob = site_box(P, f, SHOP_SITE, out + (deep ? 0.6 : 0.0));
                        if (support_hit(S, &ob, 0.05)) continue;
                    }
                    {   /* other sites: bays side by side may stand close; none in another's way (across a lane) */
                        const OBB box = site_box(B, f, SHOP_SITE, R + out), way = way_box(B, f, SHOP_SITE, R + out, walk);
                        int hit = 0;
                        for (int o = 0; o < S->n && !hit; ++o)
                            hit = obb_hit(&box, &S->box[o], S->spot[o].space == SPACE_SHOP ? 0.0 : V_GAP_MIN) ||
                                  obb_hit(&box, &S->way[o], 0.02) || obb_hit(&way, &S->box[o], 0.02);
                        if (hit) continue;
                    }
                    VendorSpot sp;
                    memset(&sp, 0, sizeof(sp));
                    sp.x = B.x; sp.z = B.z; sp.fx = (float)f.x; sp.fz = (float)f.z;
                    sp.site.w = SHOP_SITE; sp.site.d = R + (float)out;
                    sp.y = floorY;
                    sp.ground = city_ground_y(P.x, P.z);
                    sp.walk = (float)walk;
                    sp.site.ceiling = head;
                    sp.site.affluence = aff;
                    sp.site.hostWall = 1;
                    sp.site.archetype = VA_CANOPY; sp.site.trade = -1;
                    sp.site.recess = R;
                    sp.site.deep = (uint8_t)deep;
                    sp.bseed = b->seed; sp.bsec = (uint8_t)si; sp.bface = (uint8_t)k; sp.bseg = (uint8_t)seg;
                    sp.head = floorY + head - base;
                    keep_site(S, &sp, SPACE_SHOP, hash_combine(grp, (uint32_t)(si * 4096 + k * 64 + seg)), 0, 0);
                    prev = 1;
                }
            }
        }
    }
}

/* SF3 (the user, on the second board, in place of the corner bar outside a building's corner): a building's lit
 * corner shop (the bay round a square convex corner, one shop seen from both faces: city_shop_hash) taken over by the
 * inset corner unit (noodle_bar.h NbPlan.inset): opened up on both faces, the corner left on its pier, the L counter
 * along both inside the building lines and stools all along both. About one lit corner shop in two where the
 * district is poorest, one in five where richest. The front face (F: the site's frame, its back R inside it) is
 * either face, the other tried if the first won't take it; the side face's opening runs from the pier back to the
 * kitchen's lining, the rest of its bay shut (vendor_stall_opening). Both faces at least 6.5 m (whole corner bays,
 * keyed as one shop); the floor at the sidewalk's at the openings' ends; the room inside the section; no door in
 * either opening; the sidewalk in front of both clear. */
#define INSET_OUT 1.0   /* the way kept clear in front of each opening */

/* A face's opening (u0..u1 along it) and the ground in front of it clear: the block's sidewalk or a lot open to the
 * street (lot_open: not a yard shut in behind the buildings), nothing solid, no lamp, column, doorway or crossing; the
 * ground just outside its ends (ga, gb). */
static int inset_face_clear(SGen *S, const Building *b, const EdgeInfo *e, float u0, float u1, float *ga, float *gb) {
    const float ox = -e->ez, oz = e->ex;   /* out of the face, local */
    const int nu = (int)ceil((u1 - u0) / SAMPLE), nz = (int)ceil(INSET_OUT / SAMPLE);
    int lot = 0;
    for (int j = 0; j <= nz; ++j)
        for (int i = 0; i <= nu; ++i) {
            const float u = u0 + (u1 - u0) * (float)i / (float)nu, o = 0.15f + (float)(INSET_OUT - 0.15) * (float)j / (float)nz;
            const DVec2 p = to_world(b, e->ax + e->ex * u + ox * o, e->az + e->ez * u + oz * o);
            const int gk = ground_at(S, p.x, p.z);
            if ((gk != GK_SIDEWALK && gk != GK_LOT) || solid(S, p.x, p.z, 0.02f) || lamp_near(S, p.x, p.z, 0.0) || in_doorway(S, p.x, p.z)) return 0;
            lot |= gk == GK_LOT;
            for (int q = 0; q < S->nd; ++q)
                if (city_drop_contains(&S->dk[q], p.x, p.z, 0.3)) return 0;
            if (j == 0 && (i == 0 || i == nu)) *(i ? gb : ga) = city_ground_y(p.x, p.z);
        }
    const float um = 0.5f * (u0 + u1);
    const DVec2 M = to_world(b, e->ax + e->ex * um, e->az + e->ez * um), O = to_world(b, e->ax + e->ex * um + ox, e->az + e->ez * um + oz);
    const DVec2 f = dsub(O, M), t = dv(f.z, -f.x);
    if (lot && !lot_open(S, dadd(M, dscale(f, INSET_OUT + 0.3)), t, f, -1, 1)) return 0;
    const OBB ob = site_box(M, f, u1 - u0, INSET_OUT);
    return !support_hit(S, &ob, 0.05);
}

static void corner_units(SGen *S) {
    ChunkGen *g = S->g;
    const float P = NB_INSET_PIER, LIN = NB_INSET_LINING;
    for (int q = 0; q < S->nbl; ++q) {
        const int i = S->bl[q];
        const Building *b = &g->bld[i];
        if (!b->shop) continue;
        const BGraph *bg = chunk_gen_graph(g, i);
        if (bgen_industrial(bg) || bg->groundMode != GF_RETAIL || bg->archetype == ARCH_ARCOLOGY) continue;
        const float base = building_base(b), headL = G0 + bg->groundH - 1.4f;
        const uint32_t grp = hash_combine(b->seed, 0xC042u);   /* each its own group, keyed by its corner */
        for (int si = 0; si < bg->nsec; ++si) {
            const BSection *s = &bg->sec[si];
            if (!bsec_grounded(s) || !bsec_facade(s) || bsec_open(s) || relief_on(bg, s)) continue;
            const BLoop *l = &s->foot.outer;
            for (int ka = 0; ka < l->n; ++ka) {
                const FaceKey fa = face_key(l, ka, 0);
                if (!fa.endConvex) continue;   /* the corner at face ka's end, kb's start */
                const int kb = (ka + 1) % l->n;
                const FaceKey fb = face_key(l, kb, 0);
                const EdgeInfo ea = edge_of(l, ka), eb = edge_of(l, kb);
                if (ea.len < 6.5f || eb.len < 6.5f || fabsf(ea.ex * eb.ex + ea.ez * eb.ez) > 0.05f) continue;
                const uint32_t h = city_shop_hash(b->seed, si, fb.face, 0, eb.len, fb.next, fb.endConvex);
                if (!city_shop_lit(h, b->shop)) continue;
                const DVec2 C = to_world(b, eb.ax, eb.az);
                const float aff = plan_affluence(C.x, C.z);
                if (hash_float(hash_combine(h, 0xC0A1u)) >= lerpf(1.0f / 2.0f, 1.0f / 5.0f, aff)) continue;
                const int lastA = (int)floorf((ea.len - 0.01f) / SHOP_BAY);
                const float R = 3.3f + 0.45f * hash_float(hash_combine(h, 0xC0A2u));
                const int first = hash_float(hash_combine(h, 0xC0A3u)) < 0.5f;
                for (int tryF = 0; tryF < 2; ++tryF) {
                    const int onA = (tryF == 0) == first;   /* F is face ka (the corner at its end: side +1), else kb (-1) */
                    const int side = onA ? 1 : -1;
                    const int kF = onA ? ka : kb, kS = onA ? kb : ka;
                    const EdgeInfo *E = onA ? &ea : &eb, *ES = onA ? &eb : &ea;
                    /* F's opening (the bay's jamb to the pier), S's (the pier back to the lining), along each face */
                    const float uj = onA ? SHOP_BAY * (float)lastA + 0.3f : SHOP_BAY - 0.3f, uc = onA ? ea.len : 0.0f;
                    const float fu0 = onA ? uj : P, fu1 = onA ? uc - P : uj;
                    const float su0 = onA ? P : ea.len - (R - LIN), su1 = onA ? R - LIN : ea.len - P;
                    if (fu1 - fu0 < 3.45f) continue;
                    if (!onA && su0 < SHOP_BAY * (float)lastA + 0.3f) continue;   /* S's opening within its corner bay */
                    /* no door in either opening */
                    int door = 0;
                    for (int di = 0; di < bg->ndoor && !door; ++di) {
                        const BDoor *d = &bg->door[di];
                        if (d->section != si) continue;
                        if (d->face == kF) door = d->u + 0.5f * d->width + 0.15f > fu0 && d->u - 0.5f * d->width - 0.15f < fu1;
                        if (d->face == kS && !door) door = d->u + 0.5f * d->width + 0.15f > su0 && d->u - 0.5f * d->width - 0.15f < su1;
                    }
                    if (door) continue;
                    /* the sidewalk in front of both clear; the floor level across their ends */
                    float g0, g1, g2, g3;
                    if (!inset_face_clear(S, b, E, fu0, fu1, &g0, &g1) || !inset_face_clear(S, b, ES, su0, su1, &g2, &g3)) continue;
                    const float glo = minf(minf(g0, g1), minf(g2, g3)), ghi = maxf(maxf(g0, g1), maxf(g2, g3));
                    const float floorY = glo + SIDEWALK_HEIGHT, foot = base + s->y0;
                    if (ghi - glo > 0.3f || floorY < foot - 0.6f) continue;
                    float head = base + headL - floorY;
                    if (head < 3.15f) continue;
                    head = minf(head, VENDOR_TOP_MAX - 0.02f);
                    /* the room within the section */
                    const float um = 0.5f * (uj + uc), W = fabsf(uc - uj) + 0.1f;
                    int inside = 1;
                    const float us[3] = { uj, um, uc - (float)side * 0.1f };
                    for (int c = 0; c < 3 && inside; ++c)
                        for (int r = 0; r < 2 && inside; ++r) {
                            const float z = r ? R + 0.1f : 0.5f * R;
                            inside = bfoot_contains(&s->foot, E->ax + E->ex * us[c] + E->ez * z, E->az + E->ez * us[c] - E->ex * z);
                        }
                    if (!inside) continue;
                    const DVec2 Pm = to_world(b, E->ax + E->ex * um, E->az + E->ez * um);
                    const DVec2 f = dv(b->c * -E->ez - b->s * E->ex, b->s * -E->ez + b->c * E->ex);   /* out of F */
                    const DVec2 B = dsub(Pm, dscale(f, R));
                    {   /* other sites: none in it, or in the way in front of either opening */
                        const OBB box = site_box(B, f, W, R + 0.05), way = way_box(B, f, W, R + 0.05, INSET_OUT);
                        const float sm = 0.5f * (su0 + su1);
                        const DVec2 Ms = to_world(b, ES->ax + ES->ex * sm, ES->az + ES->ez * sm);
                        const DVec2 fs = dv(b->c * -ES->ez - b->s * ES->ex, b->s * -ES->ez + b->c * ES->ex);
                        const OBB ways = site_box(Ms, fs, su1 - su0, INSET_OUT);
                        int hit = 0;
                        for (int o = 0; o < S->n && !hit; ++o)
                            hit = obb_hit(&box, &S->box[o], S->spot[o].space == SPACE_SHOP || S->spot[o].space == SPACE_CORNER ? 0.0 : V_GAP_MIN) ||
                                  obb_hit(&box, &S->way[o], 0.02) || obb_hit(&way, &S->box[o], 0.02) || obb_hit(&ways, &S->box[o], 0.02);
                        if (hit) continue;
                    }
                    VendorSpot sp;
                    memset(&sp, 0, sizeof(sp));
                    sp.x = B.x; sp.z = B.z; sp.fx = (float)f.x; sp.fz = (float)f.z;
                    sp.site.w = W; sp.site.d = R + 0.05f;
                    sp.y = floorY;
                    sp.ground = city_ground_y(Pm.x, Pm.z);
                    sp.walk = (float)INSET_OUT;
                    sp.site.ceiling = head;
                    sp.site.affluence = aff;
                    sp.site.hostWall = 1;
                    sp.site.archetype = VA_CANOPY; sp.site.trade = -1;
                    sp.site.recess = R;
                    sp.site.corner = (int8_t)side;
                    sp.bseed = b->seed; sp.bsec = (uint8_t)si;
                    sp.bface = (uint8_t)kF; sp.bseg = (uint8_t)(onA ? lastA : 0);
                    sp.bface2 = (uint8_t)kS; sp.bseg2 = (uint8_t)(onA ? 0 : lastA);
                    sp.head = floorY + head - base;
                    keep_site(S, &sp, SPACE_CORNER, hash_combine(grp, (uint32_t)(si * 64 + ka)), 0, 0);
                    break;
                }
            }
        }
    }
}

/* SF3 (the user, on the first board): the shop's neon board on a recessed stall's header panel, from the stall's
 * plan (the hood at the building line under the panel, the head over it): its extent over the stall's floor. 0 if
 * it has none there (a deep unit keeps its board on the facade, over its shopfront). */
int vendor_stall_board(const VendorSpot *s, float *y0, float *y1) {
    if ((s->space != SPACE_SHOP && s->space != SPACE_CORNER) || s->site.deep || s->site.recess <= 0.0f) return 0;
    NbPlan p;
    const int corner = s->site.corner > 0 ? NB_SITE_CORNER_POS : s->site.corner < 0 ? NB_SITE_CORNER_NEG : 0;
    nb_plan_site(&p, s->site.w, s->site.d, s->site.ceiling, s->site.seed, -1, VT_NOODLE, s->site.affluence, NB_SITE_HOST | corner, s->site.recess);
    const float lo = p.panel + 0.06f, hi = p.head - 0.12f;
    if (hi - lo < 0.25f) return 0;
    const float bh = minf(0.95f, hi - lo), mid = 0.5f * (lo + hi);
    *y0 = mid - 0.5f * bh; *y1 = mid + 0.5f * bh;
    return 1;
}

const VendorSpot *vendor_stall_at(const VendorSpot *s, int n, uint32_t bseed, int sec, int face, int seg) {
    for (int i = 0; i < n; ++i) {
        if ((s[i].space != SPACE_SHOP && s[i].space != SPACE_CORNER) || s[i].bseed != bseed || s[i].bsec != sec) continue;
        if (s[i].bface == face && s[i].bseg == seg) return &s[i];
        if (s[i].space == SPACE_CORNER && s[i].bface2 == face && s[i].bseg2 == seg) return &s[i];
    }
    return NULL;
}

int vendor_stall_opening(const VendorSpot *v, int face, int seg, float faceLen, float *u0, float *u1, float (*sh)[2], int *nsh) {
    *nsh = 0;
    if (v->space == SPACE_SHOP) {
        if (v->bface != face || v->bseg != seg) return 0;
        *u0 = (float)seg * SHOP_BAY + 0.3f; *u1 = (float)seg * SHOP_BAY + SHOP_OPEN + 0.3f;
        return 1;
    }
    if (v->space != SPACE_CORNER) return 0;
    const float P = NB_INSET_PIER, back = v->site.recess - NB_INSET_LINING;
    /* (the pier's faces are the building's wall, its shop window gone: shut too) */
    if (v->bface == face && v->bseg == seg) {   /* the front face: the bay's jamb to the pier */
        if (v->site.corner > 0) { *u0 = (float)seg * SHOP_BAY + 0.3f; *u1 = faceLen - P; sh[0][0] = *u1; sh[0][1] = faceLen; }
        else                    { *u0 = P; *u1 = SHOP_BAY - 0.3f; sh[0][0] = 0.0f; sh[0][1] = P; }
        *nsh = 1;
        return 1;
    }
    if (v->bface2 == face && v->bseg2 == seg) {   /* the side face: the pier back to the lining, the rest of the bay shut */
        if (v->site.corner > 0) { *u0 = P; *u1 = back; sh[0][0] = 0.0f; sh[0][1] = P; sh[1][0] = back; sh[1][1] = SHOP_BAY; }
        else                    { *u0 = faceLen - back; *u1 = faceLen - P; sh[0][0] = *u1; sh[0][1] = faceLen; sh[1][0] = (float)seg * SHOP_BAY; sh[1][1] = *u0; }
        *nsh = 2;
        return 1;
    }
    return 0;
}

/* SF3: a shop bay's stall or an inset corner unit facing a lot is planned before the rows, which may then close the
 * only way out of the lot in front of it (a lane's stalls across a slot's mouth): with every site planned, those whose
 * way out is shut are dropped (each its own group; dropping one only frees ground for the rest). */
static int shut_in(SGen *S, DVec2 front, DVec2 f, int self, int street) {
    const DVec2 q = dadd(front, dscale(f, 0.3));
    return ground_at(S, q.x, q.z) == GK_LOT && !lot_open(S, q, dv(f.z, -f.x), f, self, street);
}
static void prune_shut_in(SGen *S) {
    uint8_t drop[MAX_BLOCK_SPOTS];
    for (int i = 0; i < S->n; ++i) {   /* (all judged against every site, then the dropped taken out) */
        const VendorSpot *v = &S->spot[i];
        const DVec2 B = dv(v->x, v->z), f = dv(v->fx, v->fz), r = dv(f.z, -f.x);
        drop[i] = 0;
        if (v->space == SPACE_SHOP) drop[i] = (uint8_t)shut_in(S, dadd(B, dscale(f, v->site.d)), f, i, v->site.deep);
        else if (v->space == SPACE_CORNER) {   /* its front face's way out, and its side face's */
            const double sd = v->site.corner, xb = sd * (0.5 * v->site.w - 0.05), zm = 0.5 * (NB_INSET_LINING + v->site.recess - NB_INSET_PIER);
            drop[i] = (uint8_t)(shut_in(S, dadd(B, dscale(f, v->site.recess + INSET_OUT)), f, i, 1) ||
                                shut_in(S, dadd(B, dadd(dscale(r, xb + sd * INSET_OUT), dscale(f, zm))), dscale(r, sd), i, 1));
        }
    }
    int m = 0;
    for (int i = 0; i < S->n; ++i) {
        if (drop[i]) continue;
        if (m != i) { S->spot[m] = S->spot[i]; S->box[m] = S->box[i]; S->way[m] = S->way[i]; }
        m++;
    }
    S->n = m;
}

/* ---------------------------------------------------------------- a block --- */

static void plan_block(SGen *S, ChunkGen *g, int bi) {
    const Block *bk = &g->pa->blocks[bi];
    S->g = g; S->bi = bi; S->bk = bk;
    S->n = S->nkeep = S->nbl = S->nvault = 0;
    for (int i = 0; i < g->nb && S->nbl < 256; ++i)
        if (g->bblk[i] == bi) S->bl[S->nbl++] = i;
    S->nd = city_block_drops(g->pa, bk, S->dk, MAX_BLOCK_DROPS);
    S->nsp = city_block_spaces(bk, S->sp, MAX_BLOCK_SPACES);
    DVec2 cen = poly_centroid(bk->lot.n ? &bk->lot : &bk->curb);
    S->aff = plan_affluence(cen.x, cen.z);
    /* the lamp posts on its sidewalks, from its roads (not g's, which reach only so far round the chunk) */
    S->nl = 0;
    for (int ri = 0; ri < g->pa->nroads && S->nl < 96; ++ri) {
        Lamp lt[64];
        const Road *rd = &g->pa->roads[ri];
        double reach = rd->at.half + rd->at.walk + 2.0;
        if (fmax(rd->a.x, rd->b.x) < bk->x0 - reach || fmin(rd->a.x, rd->b.x) > bk->x1 + reach ||
            fmax(rd->a.z, rd->b.z) < bk->z0 - reach || fmin(rd->a.z, rd->b.z) > bk->z1 + reach) continue;
        int n = city_road_lamps(g->pa, ri, bk->x0 - 1.0, bk->z0 - 1.0, bk->x1 + 1.0, bk->z1 + 1.0, lt, 64);
        for (int k = 0; k < n && S->nl < 96; ++k)
            if (poly_contains(&bk->curb, lt[k].x, lt[k].z, 0.5)) S->lamp[S->nl++] = lt[k];
    }
    /* SF3: the shop bays' stalls and the buildings' corner bars first, so the rows and the kerb pitches keep clear */
    shop_stalls(S);
    corner_units(S);
    for (int k = 0; k < S->nsp; ++k) fill_space(S, &S->sp[k]);
    CitySpace court;
    if (S->nsp < MAX_BLOCK_SPACES + 1 && find_court(S, &court)) {
        S->sp[S->nsp] = court;
        fill_space(S, &S->sp[S->nsp]);
    }
    for (int i = 0; i < bk->curb.n; ++i) kerb_pitches(S, i);
    prune_shut_in(S);
    /* each group's size */
    for (int a = 0; a < S->n; ++a) {
        int m = 0;
        for (int b = 0; b < S->n; ++b) m += S->spot[b].group == S->spot[a].group;
        S->spot[a].members = (uint16_t)m;
    }
}

/* city_internal.h */
int city_vendor_spots(ChunkGen *g, double x0, double z0, double x1, double z1, int touch, VendorSpot *out, int max) {
    SGen *S = (SGen *)malloc(sizeof(SGen));
    if (!S) fatal("out of memory placing vendors");
    int n = 0;
    for (int bi = 0; bi < g->pa->nblocks; ++bi) {
        const Block *bk = &g->pa->blocks[bi];
        if (bk->x1 < x0 || bk->x0 > x1 || bk->z1 < z0 || bk->z0 > z1) continue;
        if (bk->zone == ZONE_PARK || bk->zone == ZONE_RURAL) continue;
        plan_block(S, g, bi);
        for (int k = 0; k < S->n && n < max; ++k) {
            const VendorSpot *s = &S->spot[k];
            double mx, mz;
            vendor_spot_middle(s, &mx, &mz);
            int in;
            if (touch == VSPOTS_BLOCKS) in = 1;
            else if (touch == VSPOTS_OWNED) in = mx >= x0 && mx < x1 && mz >= z0 && mz < z1;
            else {
                OBB r;
                r.c = dv(0.5 * (x0 + x1), 0.5 * (z0 + z1)); r.t = dv(1, 0); r.n = dv(0, 1);
                r.hu = 0.5 * (x1 - x0); r.hn = 0.5 * (z1 - z0);
                in = obb_hit(&r, &S->box[k], 0.0) || obb_hit(&r, &S->way[k], 0.0);
            }
            if (in) out[n++] = *s;
        }
    }
    free(S);
    return n;
}

int city_vendor_vaults(ChunkGen *g, double x0, double z0, double x1, double z1, CityVault *out, int max) {
    SGen *S = (SGen *)malloc(sizeof(SGen));
    if (!S) fatal("out of memory placing vaults");
    int n = 0;
    for (int bi = 0; bi < g->pa->nblocks; ++bi) {
        const Block *bk = &g->pa->blocks[bi];
        if (bk->x1 < x0 || bk->x0 > x1 || bk->z1 < z0 || bk->z0 > z1) continue;
        if (bk->zone == ZONE_PARK || bk->zone == ZONE_RURAL) continue;
        int lane = 0, nsp = city_block_spaces(bk, S->sp, MAX_BLOCK_SPACES);
        for (int k = 0; k < nsp && !lane; ++k) lane = S->sp[k].kind == SPACE_LANE;
        if (!lane) continue;
        plan_block(S, g, bi);
        for (int k = 0; k < S->nvault && n < max; ++k) out[n++] = S->vault[k];
    }
    free(S);
    return n;
}

int city_chunk_vaults(int cx, int cz, CityVault *out, int max) {
    ChunkGen g;
    chunk_gen_init(&g, cx, cz, PROPS_PLAN_PAD, PROPS_BUILD_PAD, PROPS_LAMP_PAD);
    const int n = city_vendor_vaults(&g, g.ox, g.oz, g.ox + CHUNK_SIZE, g.oz + CHUNK_SIZE, out, max);
    chunk_gen_free(&g);
    return n;
}

const VendorSpot *chunk_gen_stalls(ChunkGen *g, int *n) {
    if (!g->stallDone) {
        g->stallDone = 1;
        VendorSpot *all = (VendorSpot *)malloc(sizeof(VendorSpot) * 1024);
        if (!all) fatal("out of memory placing stalls");
        const int na = city_vendor_spots(g, g->ox - 16.0, g->oz - 16.0, g->ox + CHUNK_SIZE + 16.0, g->oz + CHUNK_SIZE + 16.0, VSPOTS_BLOCKS, all, 1024);
        int m = 0;
        for (int i = 0; i < na; ++i) if (all[i].space == SPACE_SHOP || all[i].space == SPACE_CORNER) all[m++] = all[i];
        g->stall = all; g->nstall = m;
    }
    *n = g->nstall;
    return g->stall;
}

int city_chunk_vendor_spots(int cx, int cz, VendorSpot *out, int max) { return city_vendor_spots_near(cx, cz, 0.0, out, max); }

int city_vendor_spots_near(int cx, int cz, double grow, VendorSpot *out, int max) {
    ChunkGen g;
    chunk_gen_init(&g, cx, cz, PROPS_PLAN_PAD, PROPS_BUILD_PAD + grow, PROPS_LAMP_PAD);
    int n = city_vendor_spots(&g, g.ox - grow, g.oz - grow, g.ox + CHUNK_SIZE + grow, g.oz + CHUNK_SIZE + grow, VSPOTS_OWNED, out, max);
    chunk_gen_free(&g);
    return n;
}
