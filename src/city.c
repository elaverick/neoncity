#include "city.h"
#include "building.h"
#include "hash.h"
#include "mathlib.h"

/* ===========================================================================
 * Shared rules
 * ========================================================================= */

/* Conservative: density can rise by at most ~0.2 between the chunk centre and
 * any building centre in it, and buildings are clamped to this anyway. */
float city_height_cap(int cx, int cz) {
    float d = plan_density((cx + 0.5) * (double)CHUNK_SIZE, (cz + 0.5) * (double)CHUNK_SIZE) + 0.22f;
    if (d >= 0.92f) return 800.0f;
    return 140.0f + 420.0f * minf(d, 1.0f);
}

/* Facade rhythm shared with shaders/common.glsl */
static uint32_t style_hash(uint32_t seed) { return hash_u32(seed ^ 0x5f3759dfu); }
float city_floor_height(uint32_t seed)  { return 3.3f + 0.9f * hash_float(hash_combine(style_hash(seed), 1u)); }
float city_ground_height(uint32_t seed) { return 4.6f + 1.4f * hash_float(hash_combine(style_hash(seed), 2u)); }

/* Storefronts are 6 m bays along each ground-floor face; shaders/city.frag
 * uses the same hash so lit shops, their signs and their light spill agree.
 * Faces are numbered within their section; the bay that wraps a convex
 * corner is keyed as the next face's first, so a corner shop is one shop
 * seen from both sides. */
uint32_t city_shop_hash(uint32_t seed, int section, int face, int seg, float faceWidth, int nextFace, int endShare) {
    int last = (int)floorf((faceWidth - 0.01f) / 6.0f);
    if (endShare && seg >= last && last > 0) { face = nextFace; seg = 0; }
    return hash3i(WORLD_SEED ^ 0x5B0Au, (int32_t)seed, section * 64 + face, seg);
}
int city_shop_lit(uint32_t h, int level) { return hash_float(h) < (float)level * 0.125f; }

/* ===========================================================================
 * 2D helpers
 * ========================================================================= */

static DVec2 dv(double x, double z) { DVec2 r = { x, z }; return r; }
static DVec2 dsub(DVec2 a, DVec2 b) { return dv(a.x - b.x, a.z - b.z); }
static DVec2 dadd(DVec2 a, DVec2 b) { return dv(a.x + b.x, a.z + b.z); }
static DVec2 dscale(DVec2 a, double s) { return dv(a.x * s, a.z * s); }
static double ddot(DVec2 a, DVec2 b) { return a.x * b.x + a.z * b.z; }
static double dlen(DVec2 a) { return sqrt(a.x * a.x + a.z * a.z); }
static DVec2 dnorm(DVec2 a) { double l = dlen(a); return l > 1e-12 ? dv(a.x / l, a.z / l) : dv(1, 0); }

static double seg_dist(DVec2 p, DVec2 a, DVec2 b) {
    DVec2 ab = dsub(b, a);
    double l2 = ddot(ab, ab);
    double t = l2 > 1e-12 ? ddot(dsub(p, a), ab) / l2 : 0.0;
    t = t < 0.0 ? 0.0 : t > 1.0 ? 1.0 : t;
    return dlen(dsub(p, dadd(a, dscale(ab, t))));
}

static DVec2 to_world(const Building *b, double lx, double lz) {
    return dv(b->ox + b->c * lx - b->s * lz, b->oz + b->s * lx + b->c * lz);
}

static int inside_building(const Building *b, double wx, double wz, double margin) {
    double dx = wx - b->ox, dz = wz - b->oz;
    double lx = b->c * dx + b->s * dz, lz = -b->s * dx + b->c * dz;
    return lx > b->foot.x0 - margin && lx < b->foot.x1 + margin && lz > b->foot.z0 - margin && lz < b->foot.z1 + margin;
}

/* ===========================================================================
 * Filling blocks with buildings
 * ========================================================================= */

/* A polygon seen in a frame (origin o, unit axes u and v). */
typedef struct Frame {
    DVec2  o, u, v;
    int    n;
    double fu[PLAN_MAX_PV], fv[PLAN_MAX_PV];
    double umin, umax, vmin, vmax;
} Frame;

static void make_frame(const Poly *p, DVec2 o, DVec2 u, Frame *F) {
    F->o = o; F->u = u; F->v = dv(-u.z, u.x);
    DVec2 c = poly_centroid(p);
    if (ddot(dsub(c, o), F->v) < 0) F->v = dv(-F->v.x, -F->v.z);
    F->n = p->n;
    F->umin = F->vmin = 1e300; F->umax = F->vmax = -1e300;
    for (int i = 0; i < p->n; ++i) {
        DVec2 d = dsub(p->v[i], o);
        F->fu[i] = ddot(d, F->u); F->fv[i] = ddot(d, F->v);
        F->umin = fmin(F->umin, F->fu[i]); F->umax = fmax(F->umax, F->fu[i]);
        F->vmin = fmin(F->vmin, F->fv[i]); F->vmax = fmax(F->vmax, F->fv[i]);
    }
}

/* u-extent of the polygon along the line v = const. */
static int frame_slice(const Frame *F, double v, double *lo, double *hi) {
    *lo = 1e300; *hi = -1e300;
    for (int i = 0; i < F->n; ++i) {
        int k = (i + 1) % F->n;
        double va = F->fv[i], vb = F->fv[k];
        if ((va - v) * (vb - v) > 0.0) continue;
        if (fabs(vb - va) < 1e-9) {
            *lo = fmin(*lo, fmin(F->fu[i], F->fu[k])); *hi = fmax(*hi, fmax(F->fu[i], F->fu[k]));
        } else {
            double u = F->fu[i] + (F->fu[k] - F->fu[i]) * (v - va) / (vb - va);
            *lo = fmin(*lo, u); *hi = fmax(*hi, u);
        }
    }
    return *lo <= *hi;
}

/* Largest frame-aligned rectangle inside a convex polygon: v from v0 (or, if
 * search is set, the best of a few starting depths) over a depth in [minD, maxD].
 * For a convex polygon the u-range shared by a band is the overlap of its two
 * boundary slices. r = { u0, v0, u1, v1 }. */
static int frame_rect(const Frame *F, int search, double v0, double minD, double maxD, double side, double minW, double r[4]) {
    double best = 0.0;
    int ok = 0;
    int starts = search ? 4 : 1;
    for (int si = 0; si < starts; ++si) {
        double a0 = search ? F->vmin + 0.5 + (F->vmax - F->vmin) * 0.12 * si : v0;
        double room = F->vmax - a0;
        if (room < minD) continue;
        double top = fmin(maxD, room);
        double l0, h0;
        if (!frame_slice(F, a0, &l0, &h0)) continue;
        for (int k = 0; k < 8; ++k) {
            double dd = minD + (top - minD) * (k + 1) / 8.0;
            double l1, h1;
            if (!frame_slice(F, a0 + dd, &l1, &h1)) continue;
            double ul = fmax(l0, l1) + side, ur = fmin(h0, h1) - side;
            if (ur - ul < minW) continue;
            double area = (ur - ul) * dd;
            if (area > best) { best = area; r[0] = ul; r[1] = a0; r[2] = ur; r[3] = a0 + dd; ok = 1; }
        }
    }
    return ok;
}

typedef struct Filler {
    const Block *bk;
    Building    *out;
    int          n, max;
    Rng          rng;
} Filler;

static Building *place(Filler *f, const Frame *F, const double r[4]) {
    double w = r[2] - r[0], d = r[3] - r[1];
    if (f->n >= f->max || w < 8.0 || d < 8.0) return NULL;
    /* Keep the footprint within the chunk overhang budget (half-diagonal <= 64 m). */
    double diag = sqrt(w * w + d * d);
    if (diag > 128.0) { w *= 128.0 / diag; d *= 128.0 / diag; }
    DVec2 c = dadd(F->o, dadd(dscale(F->u, (r[0] + r[2]) * 0.5), dscale(F->v, (r[1] + r[3]) * 0.5)));
    Building *b = &f->out[f->n];
    memset(b, 0, sizeof(*b));
    b->ox = c.x; b->oz = c.z;
    b->c = (float)F->u.x; b->s = (float)F->u.z;
    b->foot.x0 = (float)(-w * 0.5); b->foot.x1 = (float)(w * 0.5);
    b->foot.z0 = (float)(-d * 0.5); b->foot.z1 = (float)(d * 0.5);
    b->seed = hash_combine(f->bk->seed, (uint32_t)f->n + 1u);
    b->zone = f->bk->zone;
    b->density = f->bk->density;
    return b;
}

/* Clamp to the culling bound of the chunk that will own the building, and
 * work out which faces look onto a street (signs go there). */
static void commit(Filler *f, Building *b, float height, double probe) {
    int cx = (int)floor(b->ox / CHUNK_SIZE), cz = (int)floor(b->oz / CHUNK_SIZE);
    float cap = city_height_cap(cx, cz) - (b->kind == BK_MEGA ? 66.0f : 34.0f);
    b->height = maxf(minf(height, cap), 9.0f);
    if (b->kind == BK_MEGA) {
        b->frontage = SIDE_W | SIDE_E | SIDE_S | SIDE_N;
    } else {
        static const float nx[4] = { -1, 1, 0, 0 }, nz[4] = { 0, 0, -1, 1 };
        int fr = 0;
        for (int s = 0; s < 4; ++s) {
            double lx = nx[s] < 0 ? b->foot.x0 : nx[s] > 0 ? b->foot.x1 : 0.0;
            double lz = nz[s] < 0 ? b->foot.z0 : nz[s] > 0 ? b->foot.z1 : 0.0;
            DVec2 q = to_world(b, lx + nx[s] * probe, lz + nz[s] * probe);
            if (!poly_contains(&f->bk->lot, q.x, q.z, 0.0)) fr |= 1 << s;
        }
        b->frontage = fr;
    }
    f->n++;
}

typedef struct LotList { Poly p[64]; int n; } LotList;

/* Recursive split of a convex lot along the block grain (alleys near the top). */
static void split_lots(Filler *f, const Poly *p, int depth, float maxLot, DVec2 gu, LotList *out) {
    DVec2 gv = dv(-gu.z, gu.x), cen = poly_centroid(p);
    double ulo = 1e300, uhi = -1e300, vlo = 1e300, vhi = -1e300;
    for (int i = 0; i < p->n; ++i) {
        DVec2 d = dsub(p->v[i], cen);
        ulo = fmin(ulo, ddot(d, gu)); uhi = fmax(uhi, ddot(d, gu));
        vlo = fmin(vlo, ddot(d, gv)); vhi = fmax(vhi, ddot(d, gv));
    }
    double su = uhi - ulo, sv = vhi - vlo;
    int leaf = (su <= maxLot && sv <= maxLot) || depth >= 7 || out->n >= 63;
    if (!leaf) {
        int alongU = su > sv;
        if (fabs(su - sv) < 6.0) alongU = (rng_u32(&f->rng) & 1u) != 0;
        DVec2 axis = alongU ? gu : gv;
        double lo = alongU ? ulo : vlo, hi = alongU ? uhi : vhi;
        double gap = (depth <= 1 && rng_float(&f->rng) < 0.35f) ? rng_range(&f->rng, 3.0f, 5.0f) : 0.0;
        double cut = lo + (hi - lo) * rng_range(&f->rng, 0.36f, 0.64f);
        if (cut - gap * 0.5 - lo >= 9.0 && hi - cut - gap * 0.5 >= 9.0) {
            RoadAttr none;
            memset(&none, 0, sizeof(none));
            Poly a, b;
            poly_clip(p, dadd(cen, dscale(axis, cut - gap * 0.5)), axis, none, &a);
            poly_clip(p, dadd(cen, dscale(axis, cut + gap * 0.5)), dv(-axis.x, -axis.z), none, &b);
            if (a.n && b.n && poly_area(&a) > 60.0 && poly_area(&b) > 60.0) {
                split_lots(f, &a, depth + 1, maxLot, gu, out);
                split_lots(f, &b, depth + 1, maxLot, gu, out);
                return;
            }
        }
    }
    if (out->n < 64) out->p[out->n++] = *p;
}

/* Frame on the lot's longest street edge, so the building fronts the street.
 * Returns 0 (and a grain frame) for a lot with no street edge. */
static int street_frame(const Poly *lot, DVec2 grain, Frame *F) {
    int best = -1;
    double bl = 0.0;
    for (int i = 0; i < lot->n; ++i) {
        if (lot->e[i].cls == ROAD_NONE) continue;
        double l = dlen(dsub(lot->v[(i + 1) % lot->n], lot->v[i]));
        if (l > bl) { bl = l; best = i; }
    }
    if (best >= 0) make_frame(lot, lot->v[best], dnorm(dsub(lot->v[(best + 1) % lot->n], lot->v[best])), F);
    else make_frame(lot, poly_centroid(lot), grain, F);
    return best >= 0;
}

/* Downtown, neighbourhood centres and industry: lots split off the street. */
static void fill_lots(Filler *f, const Poly *land, float maxLot, DVec2 grain) {
    const Block *bk = f->bk;
    float d = bk->density;
    LotList *L = (LotList *)malloc(sizeof(LotList));
    if (!L) fatal("out of memory filling a block");
    L->n = 0;
    split_lots(f, land, 0, maxLot, grain, L);
    for (int i = 0; i < L->n; ++i) {
        Frame F;
        int street = street_frame(&L->p[i], grain, &F);
        double front, side, maxDepth;
        if (bk->zone == ZONE_INDUSTRIAL) { front = rng_range(&f->rng, 3.0f, 6.0f); side = rng_range(&f->rng, 2.0f, 4.0f); maxDepth = 60.0; }
        else if (bk->zone == ZONE_CENTRE) { front = 0.0; side = 0.0; maxDepth = 24.0; }
        else { front = rng_float(&f->rng) < 0.25f ? rng_range(&f->rng, 0.5f, 3.0f) : 0.0; side = rng_range(&f->rng, 0.0f, 0.6f); maxDepth = 42.0; }
        double r[4];
        if (!frame_rect(&F, !street, F.vmin + front + 0.02, 8.0, maxDepth, side, 8.0, r)) continue;
        Building *b = place(f, &F, r);
        if (!b) continue;
        float minDim = minf(b->foot.x1 - b->foot.x0, b->foot.z1 - b->foot.z0);
        float H;
        if (bk->zone == ZONE_INDUSTRIAL) {
            b->kind = BK_BLOCK; b->shop = 0;
            H = rng_range(&f->rng, 8.0f, 20.0f);
        } else if (bk->zone == ZONE_CENTRE) {
            b->shop = 7;
            if (minDim >= 16.0f && rng_float(&f->rng) < 0.12f) { b->kind = BK_TOWER; H = lerpf(60.0f, 120.0f + 200.0f * d, rng_float(&f->rng)); }
            else { b->kind = BK_BLOCK; H = rng_range(&f->rng, 10.0f, 26.0f); }
        } else {
            b->shop = 6;
            if (minDim >= 16.0f && rng_float(&f->rng) < 0.06f + 0.32f * d) {
                b->kind = BK_TOWER;
                H = lerpf(90.0f, 80.0f + 400.0f * d, powf(rng_float(&f->rng), 1.4f));
            } else {
                b->kind = BK_BLOCK;
                H = lerpf(14.0f, 30.0f + 70.0f * d, rng_float(&f->rng)) * (0.7f + 0.6f * rng_float(&f->rng));
                H = minf(H, minDim * 9.0f);
            }
        }
        commit(f, b, H, 4.5);
    }
    free(L);
}

/* Housing estate: free-standing slab or point blocks in rows along the grain,
 * set back from the roads behind a green verge, like an HDB precinct. */
static void fill_estate(Filler *f, DVec2 grain) {
    const Block *bk = f->bk;
    float d = bk->density;
    Poly E;
    if (!poly_inset(&bk->lot, 2, rng_range(&f->rng, 5.0f, 9.0f), &E) || poly_area(&E) < 400.0) return;
    DVec2 u = hash_float(hash_combine(bk->seed, 5u)) < 0.5f ? grain : dv(-grain.z, grain.x);
    Frame F;
    make_frame(&E, poly_centroid(&E), u, &F);
    int slabs = hash_float(hash_combine(bk->seed, 6u)) < 0.6f;
    double D, gapV, len, gapU;
    if (slabs) {
        D = rng_range(&f->rng, 14.0f, 17.0f); gapV = rng_range(&f->rng, 20.0f, 30.0f);
        len = rng_range(&f->rng, 42.0f, 72.0f); gapU = rng_range(&f->rng, 10.0f, 18.0f);
    } else {
        D = rng_range(&f->rng, 20.0f, 26.0f); gapV = rng_range(&f->rng, 18.0f, 28.0f);
        len = D; gapU = gapV;
    }
    double span = F.vmax - F.vmin;
    int rows = (int)floor((span + gapV) / (D + gapV));
    if (rows < 1) return;
    double v = F.vmin + (span - (rows * D + (rows - 1) * gapV)) * 0.5;
    for (int row = 0; row < rows; ++row, v += D + gapV) {
        double l0, h0, l1, h1;
        if (!frame_slice(&F, v, &l0, &h0) || !frame_slice(&F, v + D, &l1, &h1)) continue;
        double ul = fmax(l0, l1), ur = fmin(h0, h1);
        double x = ul + ((row & 1) ? gapU * 0.5 : 0.0);
        while (ur - x >= (slabs ? 26.0 : D)) {
            double L = slabs ? fmin(len, ur - x) : D;
            double r[4] = { x, v, x + L, v + D };
            Building *b = place(f, &F, r);
            if (!b) return;
            b->shop = 1;
            if (slabs) { b->kind = BK_BLOCK; commit(f, b, lerpf(36.0f, 120.0f, d) * rng_range(&f->rng, 0.8f, 1.15f), 16.0); }
            else       { b->kind = BK_TOWER; commit(f, b, lerpf(60.0f, 170.0f, d) * rng_range(&f->rng, 0.85f, 1.2f), 16.0); }
            x += L + gapU;
        }
    }
}

int city_block_buildings(const Block *bk, Building *out, int max) {
    Filler f;
    f.bk = bk; f.out = out; f.n = 0; f.max = max;
    f.rng = rng_make(bk->seed ^ 0xF111u);
    if (bk->lot.n < 3 || bk->zone == ZONE_PARK) return 0;
    DVec2 grain = dv(bk->gc, bk->gs);

    switch (bk->zone) {
    case ZONE_CORE: {
        /* Rare arcologies take a whole block in the densest cores. */
        if (bk->density >= 0.9f && hash_float(hash_combine(bk->seed, 7u)) < 0.07f) {
            Frame F;
            make_frame(&bk->lot, poly_centroid(&bk->lot), grain, &F);
            double r[4];
            if (frame_rect(&F, 1, 0.0, 50.0, 90.0, 1.0, 50.0, r)) {
                Building *b = place(&f, &F, r);
                if (b) {
                    int cx = (int)floor(b->ox / CHUNK_SIZE), cz = (int)floor(b->oz / CHUNK_SIZE);
                    if (city_height_cap(cx, cz) >= 800.0f) {
                        b->kind = BK_MEGA; b->shop = 5;
                        commit(&f, b, rng_range(&f.rng, 420.0f, 720.0f), 4.5);
                        return f.n;
                    }
                }
            }
        }
        /* downtown plots are larger, so towers get room for podiums, twins and gateways */
        fill_lots(&f, &bk->lot, bk->density >= 0.75f ? rng_range(&f.rng, 28.0f, 56.0f) : rng_range(&f.rng, 22.0f, 40.0f), grain);
        break;
    }
    case ZONE_CENTRE:
        fill_lots(&f, &bk->lot, rng_range(&f.rng, 14.0f, 22.0f), grain);
        break;
    case ZONE_INDUSTRIAL: {
        Poly land;
        if (poly_inset(&bk->lot, 2, 3.0, &land)) fill_lots(&f, &land, rng_range(&f.rng, 40.0f, 65.0f), grain);
        break;
    }
    default:
        fill_estate(&f, grain);
        break;
    }
    return f.n;
}

/* ===========================================================================
 * Street lamps: evenly spaced along each carriageway between its junctions,
 * on both sidewalks, skipping side-street mouths. Pure function of the plan.
 * ========================================================================= */

static int lamp_site_ok(const PlanArea *pa, int ri, DVec2 q) {
    for (int j = 0; j < pa->nroads; ++j) {
        if (j == ri) continue;
        const Road *o = &pa->roads[j];
        double pad = o->at.half + 1.5;
        if (q.x < fmin(o->a.x, o->b.x) - pad || q.x > fmax(o->a.x, o->b.x) + pad ||
            q.z < fmin(o->a.z, o->b.z) - pad || q.z > fmax(o->a.z, o->b.z) + pad) continue;
        if (seg_dist(q, o->a, o->b) < pad) return 0;
    }
    for (int k = 0; k < pa->nblocks; ++k) {
        const Block *bk = &pa->blocks[k];
        if (q.x < bk->x0 || q.x > bk->x1 || q.z < bk->z0 || q.z > bk->z1) continue;
        if (poly_contains(&bk->curb, q.x, q.z, -0.3) && !(bk->lot.n && poly_contains(&bk->lot, q.x, q.z, 0.3))) return 1;
    }
    return 0;
}

int city_road_lamps(const PlanArea *pa, int ri, double x0, double z0, double x1, double z1, Lamp *out, int max) {
    const Road *rd = &pa->roads[ri];
    DVec2 d = dsub(rd->b, rd->a);
    double len = dlen(d);
    if (len < 1.0) return 0;
    DVec2 dir = dscale(d, 1.0 / len), nrm = dv(-dir.z, dir.x);
    double s0 = rd->trimA + 5.0, s1 = len - rd->trimB - 5.0;
    if (s1 - s0 < 2.0) return 0;
    int count = (int)((s1 - s0) / LAMP_SPACING) + 1;
    double step = count > 1 ? (s1 - s0) / (count - 1) : 0.0;
    if (count == 1) s0 = (s0 + s1) * 0.5;
    double off = rd->at.half + 0.7;
    int n = 0;
    for (int k = 0; k < count; ++k)
        for (int side = -1; side <= 1; side += 2) {
            DVec2 q = dadd(dadd(rd->a, dscale(dir, s0 + step * k)), dscale(nrm, off * side));
            if (q.x < x0 || q.x >= x1 || q.z < z0 || q.z >= z1 || n >= max) continue;
            if (!lamp_site_ok(pa, ri, q)) continue;
            Lamp *L = &out[n++];
            L->x = q.x; L->z = q.z;
            L->dx = (float)(-nrm.x * side); L->dz = (float)(-nrm.z * side);
            L->type = rd->at.lamp;
        }
    return n;
}

/* ===========================================================================
 * Everything near one chunk
 * ========================================================================= */

typedef struct ChunkGen {
    int       cx, cz;
    double    ox, oz;       /* chunk origin, world */
    PlanArea *pa;
    Building *bld;
    int      *bblk;         /* block index of each building */
    int       nb, capb;
    Lamp     *lamp;
    int       nl, capl;
    BGraph  **graph;        /* building graphs, generated on demand */
} ChunkGen;

/* planPad: plan area margin. Buildings are generated for blocks within
 * buildPad of the chunk and lamps within lampPad; lampPad must stay 20 m
 * inside planPad so every road and kerb a lamp is checked against is known. */
static void gen_init(ChunkGen *g, int cx, int cz, double planPad, double buildPad, double lampPad) {
    const double P = CHUNK_SIZE;
    memset(g, 0, sizeof(*g));
    g->cx = cx; g->cz = cz;
    g->ox = cx * P; g->oz = cz * P;
    g->pa = (PlanArea *)malloc(sizeof(PlanArea));
    if (!g->pa) fatal("out of memory generating a chunk");
    plan_area(g->ox - planPad, g->oz - planPad, g->ox + P + planPad, g->oz + P + planPad, g->pa);

    Building tmp[256];
    for (int bi = 0; bi < g->pa->nblocks; ++bi) {
        const Block *bk = &g->pa->blocks[bi];
        if (bk->x1 < g->ox - buildPad || bk->x0 > g->ox + P + buildPad ||
            bk->z1 < g->oz - buildPad || bk->z0 > g->oz + P + buildPad) continue;
        int n = city_block_buildings(bk, tmp, 256);
        for (int i = 0; i < n; ++i) {
            if (g->nb == g->capb) {
                g->capb = g->capb ? g->capb * 2 : 256;
                g->bld = (Building *)realloc(g->bld, sizeof(Building) * g->capb);
                g->bblk = (int *)realloc(g->bblk, sizeof(int) * g->capb);
                if (!g->bld || !g->bblk) fatal("out of memory generating a chunk");
            }
            g->bld[g->nb] = tmp[i];
            g->bblk[g->nb++] = bi;
        }
    }
    Lamp lt[128];
    for (int ri = 0; ri < g->pa->nroads; ++ri) {
        int n = city_road_lamps(g->pa, ri, g->ox - lampPad, g->oz - lampPad, g->ox + P + lampPad, g->oz + P + lampPad, lt, 128);
        for (int i = 0; i < n; ++i) {
            if (g->nl == g->capl) {
                g->capl = g->capl ? g->capl * 2 : 128;
                g->lamp = (Lamp *)realloc(g->lamp, sizeof(Lamp) * g->capl);
                if (!g->lamp) fatal("out of memory generating a chunk");
            }
            g->lamp[g->nl++] = lt[i];
        }
    }
}

static const BGraph *gen_graph(ChunkGen *g, int i) {
    if (!g->graph) {
        g->graph = (BGraph **)calloc((size_t)(g->nb ? g->nb : 1), sizeof(BGraph *));
        if (!g->graph) fatal("out of memory generating a chunk");
    }
    if (!g->graph[i]) {
        g->graph[i] = (BGraph *)malloc(sizeof(BGraph));
        if (!g->graph[i]) fatal("out of memory generating a chunk");
        bgen_build(&g->bld[i], g->graph[i], NULL);
    }
    return g->graph[i];
}

static void gen_free(ChunkGen *g) {
    if (g->graph) for (int i = 0; i < g->nb; ++i) free(g->graph[i]);
    free(g->graph);
    free(g->pa); free(g->bld); free(g->bblk); free(g->lamp);
    memset(g, 0, sizeof(*g));
}

static int owns(const ChunkGen *g, double x, double z) {
    return (int)floor(x / CHUNK_SIZE) == g->cx && (int)floor(z / CHUNK_SIZE) == g->cz;
}

void city_layout(int cx, int cz, BlockLayout *L) {
    const double P = CHUNK_SIZE;
    ChunkGen g;
    gen_init(&g, cx, cz, 25.0, 0.0, 0.0);
    memset(L, 0, sizeof(*L));
    L->cx = cx; L->cz = cz;
    L->density = plan_density((cx + 0.5) * P, (cz + 0.5) * P);
    for (int i = 0; i < g.nb && L->nbuild < MAX_BUILDINGS; ++i) {
        const Building *b = &g.bld[i];
        if (!owns(&g, b->ox, b->oz)) continue;
        L->build[L->nbuild++] = *b;
        /* what stands at street level: ground floors, piers, cores and columns */
        const BGraph *bg = gen_graph(&g, i);
        for (int si = 0; si < bg->nsec; ++si) {
            const BSection *s = &bg->sec[si];
            if (s->y0 > SIDEWALK_HEIGHT + 0.5f) continue;
            const BLoop *loops[2] = { &s->foot.outer, &s->foot.hole };
            if (L->ncoll >= MAX_COLL || L->ncollv + loops[0]->n + loops[1]->n > MAX_COLL_VERTS) break;
            CollPoly *c = &L->coll[L->ncoll++];
            c->first = L->ncollv; c->n = loops[0]->n;
            for (int k = 0; k < loops[0]->n; ++k) {
                DVec2 w = to_world(b, loops[0]->x[k], loops[0]->z[k]);
                L->collv[L->ncollv][0] = w.x; L->collv[L->ncollv][1] = w.z; L->ncollv++;
            }
            c->holeFirst = L->ncollv; c->holeN = loops[1]->n;
            for (int k = 0; k < loops[1]->n; ++k) {
                DVec2 w = to_world(b, loops[1]->x[k], loops[1]->z[k]);
                L->collv[L->ncollv][0] = w.x; L->collv[L->ncollv][1] = w.z; L->ncollv++;
            }
        }
        for (int k = 0; k < bg->nsup; ++k) {
            const BSupport *sp = &bg->sup[k];
            if (sp->type != SUP_COLUMN || sp->y0 > SIDEWALK_HEIGHT + 0.5f) continue;
            if (L->ncoll >= MAX_COLL || L->ncollv + 4 > MAX_COLL_VERTS) break;
            CollPoly *c = &L->coll[L->ncoll++];
            c->first = L->ncollv; c->n = 4; c->holeFirst = 0; c->holeN = 0;
            float h = sp->size * 0.5f;
            float cx[4] = { -h, -h, h, h }, cz[4] = { -h, h, h, -h };   /* negative turn, like every outer loop */
            for (int q = 0; q < 4; ++q) {
                DVec2 w = to_world(b, sp->x0 + cx[q], sp->z0 + cz[q]);
                L->collv[L->ncollv][0] = w.x; L->collv[L->ncollv][1] = w.z; L->ncollv++;
            }
        }
    }
    for (int i = 0; i < g.nl && L->nlamps < MAX_LAMPS; ++i)
        if (owns(&g, g.lamp[i].x, g.lamp[i].z)) L->lamp[L->nlamps++] = g.lamp[i];
    for (int i = 0; i < g.pa->nblocks && L->ncurbs < MAX_CURBS; ++i) {
        const Block *bk = &g.pa->blocks[i];
        if (bk->x1 < g.ox || bk->x0 > g.ox + P || bk->z1 < g.oz || bk->z0 > g.oz + P) continue;
        CurbPoly *c = &L->curb[L->ncurbs++];
        c->n = bk->curb.n;
        for (int k = 0; k < c->n; ++k) { c->v[k][0] = bk->curb.v[k].x; c->v[k][1] = bk->curb.v[k].z; }
    }
    gen_free(&g);
}

/* ===========================================================================
 * Mesh emission helpers
 * ========================================================================= */

void mesh_free(Mesh *m) { free(m->v); free(m->lights); free(m->sky); free(m->air); memset(m, 0, sizeof(*m)); }

static void mesh_push(Mesh *m, vec3 p, vec3 n, float u, float v, uint32_t seed, const uint8_t prm[4], uint32_t aux) {
    if (m->count == m->cap) {
        m->cap = m->cap ? m->cap * 2 : 8192;
        m->v = (Vertex *)realloc(m->v, m->cap * sizeof(Vertex));
        if (!m->v) fatal("out of memory building chunk mesh");
    }
    if (m->xf) {
        float x = p.x, z = p.z;
        p.x = m->xx + m->xc * x - m->xs * z;
        p.z = m->xz + m->xs * x + m->xc * z;
        x = n.x; z = n.z;
        n.x = m->xc * x - m->xs * z;
        n.z = m->xs * x + m->xc * z;
    }
    Vertex *o = &m->v[m->count++];
    o->pos[0] = p.x; o->pos[1] = p.y; o->pos[2] = p.z;
    o->nrm[0] = (int8_t)(n.x * 127.0f); o->nrm[1] = (int8_t)(n.y * 127.0f);
    o->nrm[2] = (int8_t)(n.z * 127.0f); o->nrm[3] = 0;
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

typedef struct Mat { uint32_t seed; uint8_t p[4]; uint32_t aux; } Mat;

static Mat mat(uint8_t id, uint32_t seed, uint8_t a, uint8_t b, uint8_t c) {
    Mat m; m.seed = seed; m.p[0] = id; m.p[1] = a; m.p[2] = b; m.p[3] = c; m.aux = 0;
    return m;
}

/* Quad a,b,c,d (in perimeter order). Winding is fixed up so the front face
 * always points along n, regardless of the order the corners were given. */
static void emit_quad(Mesh *m, vec3 a, vec3 b, vec3 c, vec3 d, vec3 n, const float uv[4][2], Mat mt) {
    vec3 p[4] = { a, b, c, d };
    int idx[4] = { 0, 1, 2, 3 };
    if (v3_dot(v3_cross(v3_sub(b, a), v3_sub(c, a)), n) < 0) { idx[1] = 3; idx[3] = 1; }
    static const int tri[6] = { 0, 1, 2, 0, 2, 3 };
    for (int i = 0; i < 6; ++i) {
        int k = idx[tri[i]];
        mesh_push(m, p[k], n, uv[k][0], uv[k][1], mt.seed, mt.p, mt.aux);
    }
}

static void emit_tri(Mesh *m, vec3 a, vec3 b, vec3 c, vec3 n, const float uv[3][2], Mat mt) {
    int k[3] = { 0, 1, 2 };
    vec3 p[3] = { a, b, c };
    if (v3_dot(v3_cross(v3_sub(b, a), v3_sub(c, a)), n) < 0) { k[1] = 2; k[2] = 1; }
    for (int i = 0; i < 3; ++i) mesh_push(m, p[k[i]], n, uv[k[i]][0], uv[k[i]][1], mt.seed, mt.p, mt.aux);
}

/* Convex polygon lying flat at height y, fan-triangulated, facing up. */
static void emit_flat(Mesh *m, const DVec2 *p, int n, float y, const float (*uv)[2], Mat mt) {
    vec3 up = v3(0, 1, 0);
    for (int i = 1; i + 1 < n; ++i) {
        int k[3] = { 0, i, i + 1 };
        vec3 a = v3((float)p[0].x, y, (float)p[0].z), b = v3((float)p[i].x, y, (float)p[i].z), c = v3((float)p[i + 1].x, y, (float)p[i + 1].z);
        if (v3_dot(v3_cross(v3_sub(b, a), v3_sub(c, a)), up) < 0) { k[1] = i + 1; k[2] = i; }
        for (int j = 0; j < 3; ++j)
            mesh_push(m, v3((float)p[k[j]].x, y, (float)p[k[j]].z), up, uv[k[j]][0], uv[k[j]][1], mt.seed, mt.p, mt.aux);
    }
}

/* Emission frames compose: a building's frame inside the chunk, a face's
 * frame inside the building (x along the face, z out of it). */
typedef struct Xf { int on; float c, s, x, z; } Xf;
static Xf xf_save(const Mesh *m) { Xf t; t.on = m->xf; t.c = m->xc; t.s = m->xs; t.x = m->xx; t.z = m->xz; return t; }
static void xf_restore(Mesh *m, Xf t) { m->xf = t.on; m->xc = t.c; m->xs = t.s; m->xx = t.x; m->xz = t.z; }
static void xf_enter(Mesh *m, float ox, float oz, float c, float s) {
    if (!m->xf) { m->xf = 1; m->xc = c; m->xs = s; m->xx = ox; m->xz = oz; return; }
    float nc = m->xc * c - m->xs * s, ns = m->xs * c + m->xc * s;
    float nx = m->xx + m->xc * ox - m->xs * oz, nz = m->xz + m->xs * ox + m->xc * oz;
    m->xc = nc; m->xs = ns; m->xx = nx; m->xz = nz;
}

enum { F_NX = 1, F_PX = 2, F_NZ = 4, F_PZ = 8, F_TOP = 16, F_BOT = 32, F_SIDES = 15, F_NOBOT = 31 };
enum { UV_FACADE = 0, UV_UNIT = 1 };

/* Vertical face uv: u = metres from the face's left edge (seen from outside),
 * v = absolute height. Tangent is cross(up, normal), matching the shader. */
static void emit_box(Mesh *m, vec3 mn, vec3 mx, int faces, Mat mt, int uvmode) {
    vec3 N[6] = { {-1,0,0}, {1,0,0}, {0,0,-1}, {0,0,1}, {0,1,0}, {0,-1,0} };
    for (int f = 0; f < 6; ++f) {
        if (!(faces & (1 << f))) continue;
        vec3 n = N[f], c[4];
        if (f == 0 || f == 1) {
            float x = f == 0 ? mn.x : mx.x;
            c[0] = v3(x, mn.y, mn.z); c[1] = v3(x, mn.y, mx.z); c[2] = v3(x, mx.y, mx.z); c[3] = v3(x, mx.y, mn.z);
        } else if (f == 2 || f == 3) {
            float z = f == 2 ? mn.z : mx.z;
            c[0] = v3(mn.x, mn.y, z); c[1] = v3(mx.x, mn.y, z); c[2] = v3(mx.x, mx.y, z); c[3] = v3(mn.x, mx.y, z);
        } else {
            float y = f == 4 ? mx.y : mn.y;
            c[0] = v3(mn.x, y, mn.z); c[1] = v3(mx.x, y, mn.z); c[2] = v3(mx.x, y, mx.z); c[3] = v3(mn.x, y, mx.z);
        }
        float uv[4][2];
        if (f < 4) {
            vec3 t = v3_cross(v3(0, 1, 0), n);
            float umin = 1e30f, umax = -1e30f;
            for (int i = 0; i < 4; ++i) { float u = v3_dot(c[i], t); umin = minf(umin, u); umax = maxf(umax, u); }
            for (int i = 0; i < 4; ++i) {
                float u = v3_dot(c[i], t) - umin;
                if (uvmode == UV_UNIT) { uv[i][0] = u / maxf(umax - umin, 1e-4f); uv[i][1] = (c[i].y - mn.y) / maxf(mx.y - mn.y, 1e-4f); }
                else { uv[i][0] = u; uv[i][1] = c[i].y; }
            }
        } else {
            for (int i = 0; i < 4; ++i) {
                if (uvmode == UV_UNIT) { uv[i][0] = (c[i].x - mn.x) / maxf(mx.x - mn.x, 1e-4f); uv[i][1] = (c[i].z - mn.z) / maxf(mx.z - mn.z, 1e-4f); }
                else { uv[i][0] = c[i].x; uv[i][1] = c[i].z; }
            }
        }
        emit_quad(m, c[0], c[1], c[2], c[3], n, uv, mt);
    }
}

/* A face in its own frame: origin at its left corner on the ground, x along it, z out of it. */
typedef struct Face { vec3 o, t, n; float width; } Face;

static Face face_local(float width) {
    Face f; f.o = v3(0, 0, 0); f.t = v3(1, 0, 0); f.n = v3(0, 0, 1); f.width = width;
    return f;
}

static vec3 face_point(const Face *f, float u, float y, float out) {
    return v3_add(v3_add(f->o, v3_scale(f->t, u)), v3_add(v3(0, y, 0), v3_scale(f->n, out)));
}

static void push_light(SkyLight **arr, uint32_t *n, uint32_t *cap, const SkyLight *L) {
    if (*n == *cap) {
        *cap = *cap ? *cap * 2 : 16;
        *arr = (SkyLight *)realloc(*arr, *cap * sizeof(SkyLight));
        if (!*arr) fatal("out of memory building chunk lights");
    }
    (*arr)[(*n)++] = *L;
}

/* A neon sign or video screen lights the rain mist (an air light) and, if it
 * can reach the cloud layer, the clouds (a sky light; src/common.h): its
 * centre and facing in chunk-local space (through the current emission
 * transform, as mesh_push), and its power, from the sign's average emitted
 * radiance (shadeNeon / shadeScreen) and its area. */
static void sign_lights(Mesh *m, vec3 c, vec3 n, float area, Mat mt) {
    int screen = mt.p[0] == MAT_SCREEN;
    float power = area * (screen ? 0.7f : 1.0f);
    float reach = sky_light_range(power);
    if (m->xf) {
        float x = c.x, z = c.z;
        c.x = m->xx + m->xc * x - m->xs * z;
        c.z = m->xz + m->xs * x + m->xc * z;
        x = n.x; z = n.z;
        n.x = m->xc * x - m->xs * z;
        n.z = m->xs * x + m->xc * z;
    }
    SkyLight L;
    L.pos[0] = c.x; L.pos[1] = c.y; L.pos[2] = c.z;
    L.power = power;
    L.nx = n.x; L.nz = n.z;
    L.seed = mt.seed;
    L.kind = screen ? SKY_KIND_SCREEN : (uint32_t)((mt.p[1] & 7) | (((mt.p[1] >> 6) & 3) << 3));
    L.kind |= AIR_SIGN << 8;
    push_light(&m->air, &m->nair, &m->capair, &L);
    if (c.y + reach >= CLOUD_LOW && c.y - reach <= CLOUD_HIGH) push_light(&m->sky, &m->nsky, &m->capsky, &L);
}

/* Flat quad parallel to a face: u in [u0,u1], y in [y0,y1], offset `out`. uv normalised 0..1. */
static void emit_face_panel(Mesh *m, const Face *f, float u0, float u1, float y0, float y1, float out, Mat mt) {
    float uv[4][2] = { {0,0}, {1,0}, {1,1}, {0,1} };
    emit_quad(m, face_point(f, u0, y0, out), face_point(f, u1, y0, out),
              face_point(f, u1, y1, out), face_point(f, u0, y1, out), f->n, uv, mt);
    if (mt.p[0] == MAT_NEON || mt.p[0] == MAT_SCREEN)
        sign_lights(m, face_point(f, 0.5f * (u0 + u1), 0.5f * (y0 + y1), out), f->n, fabsf((u1 - u0) * (y1 - y0)), mt);
}

static uint8_t q_half(float metres) { float v = metres * 2.0f + 0.5f; return (uint8_t)(v > 255 ? 255 : v); }

static void emit_emissive_box(Mesh *m, vec3 c, float half, uint8_t color, uint8_t mode, uint8_t phase) {
    emit_box(m, v3(c.x - half, c.y - half, c.z - half), v3(c.x + half, c.y + half, c.z + half),
             F_NOBOT | F_BOT, mat(MAT_EMISSIVE, 0, color, mode, phase), UV_UNIT);
}

/* Square-section beam between two points (raked struts). */
static void emit_beam(Mesh *m, vec3 a, vec3 b, float size, Mat mt) {
    vec3 ax = v3_norm(v3_sub(b, a));
    vec3 h = v3_cross(ax, v3(0, 1, 0));
    h = v3_len(h) < 1e-3f ? v3(1, 0, 0) : v3_norm(h);
    vec3 v = v3_norm(v3_cross(h, ax));
    float r = size * 0.5f;
    vec3 o[4] = { v3_add(v3_scale(h, r), v3_scale(v, r)), v3_add(v3_scale(h, -r), v3_scale(v, r)),
                  v3_add(v3_scale(h, -r), v3_scale(v, -r)), v3_add(v3_scale(h, r), v3_scale(v, -r)) };
    float uv[4][2] = { {0,0}, {1,0}, {1,1}, {0,1} };
    for (int i = 0; i < 4; ++i) {
        int j = (i + 1) & 3;
        emit_quad(m, v3_add(a, o[i]), v3_add(a, o[j]), v3_add(b, o[j]), v3_add(b, o[i]), v3_norm(v3_add(o[i], o[j])), uv, mt);
    }
}

/* ===========================================================================
 * Buildings: emitting a validated building graph (src/building.c) in the
 * building's own frame
 * ========================================================================= */

typedef struct EdgeInfo { float ax, az, ex, ez, len; } EdgeInfo;

static EdgeInfo edge_of(const BLoop *l, int k) {
    int k1 = (k + 1) % l->n;
    EdgeInfo e;
    e.ax = l->x[k]; e.az = l->z[k];
    float dx = l->x[k1] - l->x[k], dz = l->z[k1] - l->z[k];
    e.len = sqrtf(dx * dx + dz * dz);
    e.ex = e.len > 1e-6f ? dx / e.len : 1.0f;
    e.ez = e.len > 1e-6f ? dz / e.len : 0.0f;
    return e;
}

/* Face frame of edge k: x along the face (the shader's u), z out of the building. */
static float enter_face(Mesh *m, const BLoop *l, int k) {
    EdgeInfo e = edge_of(l, k);
    xf_enter(m, e.ax, e.az, e.ex, e.ez);
    return e.len;
}

static uint32_t w16(float w) { float v = w * 16.0f + 0.5f; return v < 0.0f ? 0u : v > 4095.0f ? 4095u : (uint32_t)v; }

/* Identity of face k and of the corner it ends on: faces are numbered round
 * the outer loop, then the courtyard; a convex corner's bays are shared with
 * the next face (convex: the outline turns towards the building). */
typedef struct FaceKey { int face, next, startConvex, endConvex, start90, end90; float len, prevLen, nextLen; } FaceKey;

static FaceKey face_key(const BLoop *l, int k, int base) {
    int n = l->n, kp = (k + n - 1) % n, kn = (k + 1) % n;
    EdgeInfo e = edge_of(l, k), ep = edge_of(l, kp), en = edge_of(l, kn);
    FaceKey f;
    f.face = base + k; f.next = base + kn;
    f.len = e.len; f.prevLen = ep.len; f.nextLen = en.len;
    f.startConvex = ep.ex * e.ez - ep.ez * e.ex < -0.05f;
    f.endConvex = e.ex * en.ez - e.ez * en.ex < -0.05f;
    f.start90 = f.startConvex && fabsf(ep.ex * e.ex + ep.ez * e.ez) < 0.1f;
    f.end90 = f.endConvex && fabsf(e.ex * en.ex + e.ez * en.ez) < 0.1f;
    return f;
}

/* Facade vertex data (decoded by shadeFacade in shaders/city.frag):
 *   uv.y   this face's width | the next face's width << 12, in 1/16 m (an exact integer)
 *   aux    the previous face's width | face << 12 | next face << 18 |
 *          corner flags << 24 (start/end convex, start/end square) | plant-floor code << 28
 *   params y section, z at-ground | ground mode << 1 | window << 3 | bands << 5 | fine bays << 6,
 *          w palette | shop level << 3 */
static Mat facade_mat(const Building *b, const BGraph *g, int si, const FaceKey *fk, float *packW) {
    const BSection *s = &g->sec[si];
    Mat mt = mat(MAT_FACADE, g->seed, (uint8_t)si,
                 (uint8_t)((s->y0 <= SIDEWALK_HEIGHT + 0.01f ? 1 : 0) | (g->groundMode << 1) | (s->win << 3) | (s->bands ? 32 : 0) | (g->fine ? 64 : 0)),
                 (uint8_t)(s->palette | (b->shop << 3)));
    mt.aux = w16(fk->prevLen) | (uint32_t)fk->face << 12 | (uint32_t)fk->next << 18 |
             (uint32_t)(fk->startConvex | fk->endConvex << 1 | fk->start90 << 2 | fk->end90 << 3) << 24 |
             (uint32_t)(s->plant & 15) << 28;
    *packW = (float)(w16(fk->len) | w16(fk->nextLen) << 12);
    return mt;
}

static void emit_walls(Mesh *m, const Building *b, const BGraph *g, int si) {
    const BSection *s = &g->sec[si];
    const BLoop *loops[2] = { &s->foot.outer, &s->foot.hole };
    int base = 0;
    for (int li = 0; li < 2; ++li) {
        const BLoop *l = loops[li];
        for (int k = 0; k < l->n; ++k) {
            FaceKey fk = face_key(l, k, base);
            float pw;
            Mat mt = facade_mat(b, g, si, &fk, &pw);
            Xf sv = xf_save(m);
            float len = enter_face(m, l, k);
            float uv[4][2] = { { 0, pw }, { len, pw }, { len, pw }, { 0, pw } };
            emit_quad(m, v3(0, s->y0, 0), v3(len, s->y0, 0), v3(len, s->y1, 0), v3(0, s->y1, 0), v3(0, 0, 1), uv, mt);
            xf_restore(m, sv);
        }
        base += l->n;
    }
}

static void emit_cap(Mesh *m, const BFoot *f, float y, int down, Mat mt) {
    float pts[2 * BG_MAX_PV][2];
    int tri[2 * BG_MAX_PV][3], np;
    int nt = bfoot_triangulate(f, pts, &np, tri, 2 * BG_MAX_PV);
    vec3 n = v3(0, down ? -1.0f : 1.0f, 0);
    for (int t = 0; t < nt; ++t) {
        float uv[3][2];
        vec3 p[3];
        for (int i = 0; i < 3; ++i) {
            p[i] = v3(pts[tri[t][i]][0], y, pts[tri[t][i]][1]);
            uv[i][0] = p[i].x; uv[i][1] = p[i].z;
        }
        emit_tri(m, p[0], p[1], p[2], n, uv, mt);
    }
}

/* Is (x, z) on the open roof of section si (not under anything standing on it)? */
static int roof_free(const BGraph *g, int si, float x, float z, float clear) {
    const BSection *s = &g->sec[si];
    if (!bfoot_contains(&s->foot, x, z) || bfoot_boundary_dist(&s->foot, x, z, NULL, NULL) < clear) return 0;
    for (int t = 0; t < g->nsec; ++t) {
        const BSection *o = &g->sec[t];
        if (t == si || fabsf(o->y0 - s->y1) > 0.05f) continue;
        if (bfoot_contains(&o->foot, x, z) || bfoot_boundary_dist(&o->foot, x, z, NULL, NULL) < clear) return 0;
    }
    return 1;
}

static int has_above(const BGraph *g, int si) {
    for (int t = 0; t < g->nsec; ++t)
        if (t != si && fabsf(g->sec[t].y0 - g->sec[si].y1) < 0.05f) return 1;
    return 0;
}

typedef struct Placed { float x0, z0, x1, z1; } Placed;

/* A free spot for a sx-by-sz item on the roof of si, clear of what is already there. */
static int roof_spot(const BGraph *g, int si, Rng *r, float sx, float sz, Placed *pl, int *npl, float *ox, float *oz) {
    float x0, z0, x1, z1;
    const BLoop *l = &g->sec[si].foot.outer;
    x0 = z0 = 1e30f; x1 = z1 = -1e30f;
    for (int i = 0; i < l->n; ++i) { x0 = minf(x0, l->x[i]); x1 = maxf(x1, l->x[i]); z0 = minf(z0, l->z[i]); z1 = maxf(z1, l->z[i]); }
    for (int attempt = 0; attempt < 12; ++attempt) {
        float x = rng_range(r, x0 + 1.0f, x1 - 1.0f - sx), z = rng_range(r, z0 + 1.0f, z1 - 1.0f - sz);
        if (!roof_free(g, si, x, z, 1.0f) || !roof_free(g, si, x + sx, z, 1.0f) || !roof_free(g, si, x, z + sz, 1.0f) ||
            !roof_free(g, si, x + sx, z + sz, 1.0f) || !roof_free(g, si, x + sx * 0.5f, z + sz * 0.5f, 1.0f)) continue;
        int hit = 0;
        for (int i = 0; i < *npl && !hit; ++i)
            hit = x < pl[i].x1 + 0.8f && x + sx > pl[i].x0 - 0.8f && z < pl[i].z1 + 0.8f && z + sz > pl[i].z0 - 0.8f;
        if (hit) continue;
        if (*npl < 32) { pl[*npl].x0 = x; pl[*npl].z0 = z; pl[*npl].x1 = x + sx; pl[*npl].z1 = z + sz; (*npl)++; }
        *ox = x; *oz = z;
        return 1;
    }
    return 0;
}

static void dress_roof(Mesh *m, const BGraph *g, int si, Rng *r) {
    const BSection *s = &g->sec[si];
    const BLoop *l = &s->foot.outer;
    float y = s->y1;
    float area = -bloop_area(l) - (s->foot.hole.n ? bloop_area(&s->foot.hole) : 0.0f);
    Placed pl[32];
    int npl = 0;
    float x, z;
    switch (s->roof) {
    case ROOF_PLANT: {
        int units = (int)clampf(area / 180.0f, 1.0f, 7.0f);
        for (int i = 0; i < units; ++i) {
            float sx = rng_range(r, 2.0f, 5.0f), sz = rng_range(r, 2.0f, 5.0f), sy = rng_range(r, 1.2f, 3.2f);
            if (roof_spot(g, si, r, sx, sz, pl, &npl, &x, &z))
                emit_box(m, v3(x, y, z), v3(x + sx, y + sy, z + sz), F_NOBOT, mat(MAT_METAL, g->seed + (uint32_t)i, 1, 0, 0), UV_FACADE);
        }
        if (area > 900.0f && roof_spot(g, si, r, 7.0f, 5.0f, pl, &npl, &x, &z))   /* plant room */
            emit_box(m, v3(x, y, z), v3(x + 7.0f, y + 3.4f, z + 5.0f), F_NOBOT, mat(MAT_METAL, g->seed, 0, 0, 0), UV_FACADE);
        break;
    }
    case ROOF_TANKS: {
        int n = rng_int(r, 1, 4);
        Mat metal = mat(MAT_METAL, g->seed, 1, 0, 0), legs = mat(MAT_METAL, g->seed, 2, 0, 0);
        for (int i = 0; i < n; ++i) {
            float sz = rng_range(r, 3.0f, 4.5f);
            if (!roof_spot(g, si, r, sz, sz, pl, &npl, &x, &z)) continue;
            for (int k = 0; k < 4; ++k) {
                float lx = x + ((k & 1) ? sz - 0.3f : 0.1f), lz = z + ((k & 2) ? sz - 0.3f : 0.1f);
                emit_box(m, v3(lx, y, lz), v3(lx + 0.2f, y + 1.4f, lz + 0.2f), F_SIDES, legs, UV_FACADE);
            }
            emit_box(m, v3(x, y + 1.4f, z), v3(x + sz, y + 3.9f, z + sz), F_NOBOT | F_BOT, metal, UV_FACADE);
        }
        break;
    }
    case ROOF_SOLAR: {
        Mat frame = mat(MAT_METAL, g->seed, 2, 0, 0), panel = mat(MAT_METAL, g->seed, 6, 0, 0);
        float x0 = 1e30f, x1 = -1e30f, z0 = 1e30f, z1 = -1e30f;
        for (int i = 0; i < l->n; ++i) { x0 = minf(x0, l->x[i]); x1 = maxf(x1, l->x[i]); z0 = minf(z0, l->z[i]); z1 = maxf(z1, l->z[i]); }
        int panels = 0;
        for (float pz = z0 + 1.5f; pz + 1.2f < z1 - 1.0f && panels < 160; pz += 2.6f)
            for (float px = x0 + 1.5f; px + 1.7f < x1 - 1.0f && panels < 160; px += 1.9f) {
                if (!roof_free(g, si, px, pz, 0.8f) || !roof_free(g, si, px + 1.7f, pz + 1.1f, 0.8f)) continue;
                float uv[4][2] = { {0,0}, {1,0}, {1,1}, {0,1} };
                vec3 a = v3(px, y + 0.45f, pz), b = v3(px + 1.7f, y + 0.45f, pz), c = v3(px + 1.7f, y + 0.85f, pz + 1.1f), d = v3(px, y + 0.85f, pz + 1.1f);
                emit_quad(m, a, b, c, d, v3_norm(v3(0, 1.1f, -0.4f)), uv, panel);
                emit_box(m, v3(px + 0.75f, y, pz + 0.4f), v3(px + 0.95f, y + 0.6f, pz + 0.6f), F_SIDES, frame, UV_FACADE);
                panels++;
            }
        break;
    }
    case ROOF_GARDEN: {
        emit_cap(m, &s->foot, y + 0.06f, 0, mat(MAT_LOT, 0, 1, 0, 0));
        int n = rng_int(r, 2, 6);
        for (int i = 0; i < n; ++i) {
            float sx = rng_range(r, 1.5f, 4.0f), sz = rng_range(r, 1.0f, 2.0f);
            if (roof_spot(g, si, r, sx, sz, pl, &npl, &x, &z))
                emit_box(m, v3(x, y, z), v3(x + sx, y + 0.7f, z + sz), F_NOBOT, mat(MAT_METAL, g->seed, 0, 0, 0), UV_FACADE);
        }
        break;
    }
    case ROOF_HELIPAD: {
        float cx = 0.0f, cz = 0.0f;
        for (int i = 0; i < l->n; ++i) { cx += l->x[i]; cz += l->z[i]; }
        cx /= (float)l->n; cz /= (float)l->n;
        int ok = 1;
        for (int k = 0; k < 8 && ok; ++k)
            ok = roof_free(g, si, cx + 9.0f * cosf(k * PI_F * 0.25f), cz + 9.0f * sinf(k * PI_F * 0.25f), 0.5f);
        if (!ok) break;
        BFoot pad;
        memset(&pad, 0, sizeof(pad));
        for (int k = 0; k < 8; ++k) {
            float a = -(k + 0.5f) * PI_F * 0.25f;   /* negative turn: an outer loop */
            pad.outer.x[k] = cx + 8.0f * cosf(a); pad.outer.z[k] = cz + 8.0f * sinf(a);
        }
        pad.outer.n = 8;
        emit_cap(m, &pad, y + 0.3f, 0, mat(MAT_METAL, g->seed, 2, 0, 0));
        /* the H, and edge lights */
        emit_box(m, v3(cx - 2.2f, y + 0.3f, cz - 3.0f), v3(cx - 1.6f, y + 0.36f, cz + 3.0f), F_TOP, mat(MAT_EMISSIVE, 0, 6, 0, 0), UV_UNIT);
        emit_box(m, v3(cx + 1.6f, y + 0.3f, cz - 3.0f), v3(cx + 2.2f, y + 0.36f, cz + 3.0f), F_TOP, mat(MAT_EMISSIVE, 0, 6, 0, 0), UV_UNIT);
        emit_box(m, v3(cx - 1.6f, y + 0.3f, cz - 0.3f), v3(cx + 1.6f, y + 0.36f, cz + 0.3f), F_TOP, mat(MAT_EMISSIVE, 0, 6, 0, 0), UV_UNIT);
        for (int k = 0; k < 8; ++k)
            emit_emissive_box(m, v3(cx + 8.3f * cosf(k * PI_F * 0.25f), y + 0.4f, cz + 8.3f * sinf(k * PI_F * 0.25f)), 0.12f, 5, 0, 0);
        break;
    }
    case ROOF_HIP: case ROOF_PYRAMID: {
        if (s->foot.hole.n) break;
        float cx = 0.0f, cz = 0.0f;
        for (int i = 0; i < l->n; ++i) { cx += l->x[i]; cz += l->z[i]; }
        cx /= (float)l->n; cz /= (float)l->n;
        float x0 = 1e30f, x1 = -1e30f, z0 = 1e30f, z1 = -1e30f;
        for (int i = 0; i < l->n; ++i) { x0 = minf(x0, l->x[i]); x1 = maxf(x1, l->x[i]); z0 = minf(z0, l->z[i]); z1 = maxf(z1, l->z[i]); }
        float mn = minf(x1 - x0, z1 - z0);
        int rect = l->n == 4 && fabsf((x1 - x0) * (z1 - z0) + bloop_area(l)) < 0.5f;
        if (s->roof == ROOF_HIP && rect) {
            /* ridge along the long axis */
            float h = minf(mn * 0.3f, 7.0f), half = mn * 0.5f;
            int alongX = x1 - x0 >= z1 - z0;
            vec3 r0 = alongX ? v3(x0 + half, y + h, cz) : v3(cx, y + h, z0 + half);
            vec3 r1 = alongX ? v3(x1 - half, y + h, cz) : v3(cx, y + h, z1 - half);
            vec3 c00 = v3(x0, y, z0), c10 = v3(x1, y, z0), c11 = v3(x1, y, z1), c01 = v3(x0, y, z1);
            Mat tiles = mat(MAT_ROOF, g->seed, 0, 0, 1);
            float uv4[4][2] = { {0,0}, {1,0}, {1,1}, {0,1} }, uv3[3][2] = { {0,0}, {1,0}, {0.5f,1} };
            vec3 q[4][4], t[2][3];
            if (alongX) {
                q[0][0] = c00; q[0][1] = c10; q[0][2] = r1; q[0][3] = r0;
                q[1][0] = c11; q[1][1] = c01; q[1][2] = r0; q[1][3] = r1;
                t[0][0] = c01; t[0][1] = c00; t[0][2] = r0;
                t[1][0] = c10; t[1][1] = c11; t[1][2] = r1;
            } else {
                q[0][0] = c10; q[0][1] = c11; q[0][2] = r1; q[0][3] = r0;
                q[1][0] = c01; q[1][1] = c00; q[1][2] = r0; q[1][3] = r1;
                t[0][0] = c00; t[0][1] = c10; t[0][2] = r0;
                t[1][0] = c11; t[1][1] = c01; t[1][2] = r1;
            }
            for (int i = 0; i < 2; ++i) {
                vec3 n = v3_norm(v3_cross(v3_sub(q[i][1], q[i][0]), v3_sub(q[i][3], q[i][0])));
                if (n.y < 0) n = v3_scale(n, -1.0f);
                emit_quad(m, q[i][0], q[i][1], q[i][2], q[i][3], n, uv4, tiles);
                vec3 tn = v3_norm(v3_cross(v3_sub(t[i][1], t[i][0]), v3_sub(t[i][2], t[i][0])));
                if (tn.y < 0) tn = v3_scale(tn, -1.0f);
                emit_tri(m, t[i][0], t[i][1], t[i][2], tn, uv3, tiles);
            }
        } else if (s->roof == ROOF_PYRAMID) {
            float h = minf(mn * 0.35f, 12.0f);
            vec3 apex = v3(cx, y + h, cz);
            Mat glass = mat(MAT_METAL, g->seed, 7, 0, 0);
            float uv3[3][2] = { {0,0}, {1,0}, {0.5f,1} };
            for (int k = 0; k < l->n; ++k) {
                int k1 = (k + 1) % l->n;
                vec3 a = v3(l->x[k], y, l->z[k]), b = v3(l->x[k1], y, l->z[k1]);
                vec3 n = v3_norm(v3_cross(v3_sub(b, a), v3_sub(apex, a)));
                if (n.y < 0) n = v3_scale(n, -1.0f);
                emit_tri(m, a, b, apex, n, uv3, glass);
            }
            emit_emissive_box(m, v3(cx, y + h + 0.3f, cz), 0.3f, 2, 1, 0);
        }
        break;
    }
    default:
        break;
    }
}

/* Height of the facade above a ground-floor face: follow the sections stacked over it. */
static float facade_top(const BGraph *g, int si, float px, float pz) {
    float top = g->sec[si].y1;
    for (int guard = 0; guard < g->nsec; ++guard) {
        int found = 0;
        for (int t = 0; t < g->nsec && !found; ++t)
            if (fabsf(g->sec[t].y0 - top) < 0.05f && bfoot_contains(&g->sec[t].foot, px, pz)) { top = g->sec[t].y1; found = 1; }
        if (!found) break;
    }
    return top;
}

static int face_toward(const Building *b, const EdgeInfo *e) {
    float nx = -e->ez, nz = e->ex;
    int side = fabsf(nx) > fabsf(nz) ? (nx < 0 ? SIDE_W : SIDE_E) : (nz < 0 ? SIDE_S : SIDE_N);
    return (b->frontage & side) != 0;
}

/* Street-level signs on one ground-floor face (in the face's frame). */
static void face_signs(Mesh *m, const Building *b, const BGraph *g, int si, const FaceKey *fk, float top, Rng *r) {
    const float gy = SIDEWALK_HEIGHT;
    float gh = g->groundH, fh = g->floorH;
    Face f = face_local(fk->len);
    int retail = g->groundMode == GF_RETAIL;

    /* Shop boards above most lit storefronts (6 m bays along the face). */
    if (retail && b->shop) {
        for (int seg = 0; seg * 6.0f < f.width; ++seg) {
            float u0 = seg * 6.0f + 0.4f, u1 = minf(seg * 6.0f + 6.0f, f.width) - 0.4f;
            if (u1 - u0 < 2.6f) continue;
            uint32_t h = city_shop_hash(b->seed, si, fk->face, seg, fk->len, fk->next, fk->endConvex);
            if (!city_shop_lit(h, b->shop) || hash_float(hash_combine(h, 0xB0A2u)) > 0.8f) continue;
            uint8_t col = (uint8_t)(rng_u32(r) & 7);
            uint8_t flick = (uint8_t)(rng_float(r) < 0.15f ? 1 : 0);
            emit_face_panel(m, &f, u0, u1, gy + gh - 1.3f, gy + gh - 0.35f, 0.14f,
                            mat(MAT_NEON, rng_u32(r), (uint8_t)(col | (1 << 3) | (flick << 6)), q_half(u1 - u0), q_half(0.95f)));
        }
    }

    /* Vertical blade signs sticking out over the sidewalk. */
    if (g->archetype != ARCH_ARCOLOGY && g->groundMode != GF_VOID) {
        int count = (int)(f.width / 8.0f);
        float maxH = minf(14.0f, top - (gy + gh) - 2.0f);
        for (int i = 0; i < count && maxH > 3.0f; ++i) {
            if (rng_float(r) > (retail ? 0.7f : 0.3f)) continue;
            float u = rng_range(r, 1.5f, f.width - 1.5f);
            float h = rng_range(r, 3.0f, maxH);
            float y0 = gy + gh + 0.8f + floorf(rng_range(r, 0.0f, 2.0f)) * fh;
            if (y0 + h > top - 0.5f) y0 = top - 0.5f - h;
            float depth = rng_range(r, 1.1f, 1.8f);
            float th = 0.12f;
            emit_box(m, v3(u - th, y0, 0.25f), v3(u + th, y0 + h, 0.25f + depth), F_NOBOT | F_BOT, mat(MAT_METAL, b->seed, 3, 0, 0), UV_FACADE);
            uint8_t col = (uint8_t)(rng_u32(r) & 7);
            uint8_t flick = (uint8_t)(rng_float(r) < 0.2f ? (rng_float(r) < 0.5f ? 1 : 2) : 0);
            Mat nm = mat(MAT_NEON, rng_u32(r), (uint8_t)(col | (0 << 3) | (flick << 6)), q_half(depth), q_half(h));
            float uvA[4][2] = { {0,0}, {1,0}, {1,1}, {0,1} };
            float uvB[4][2] = { {1,0}, {0,0}, {0,1}, {1,1} };
            for (int sd = -1; sd <= 1; sd += 2) {
                float uu = u + sd * (th + 0.01f);
                vec3 p0 = v3(uu, y0 + 0.1f, 0.3f), p1 = v3(uu, y0 + 0.1f, 0.2f + depth);
                vec3 p2 = v3(uu, y0 + h - 0.1f, 0.2f + depth), p3 = v3(uu, y0 + h - 0.1f, 0.3f);
                emit_quad(m, p0, p1, p2, p3, v3((float)sd, 0, 0), sd < 0 ? uvB : uvA, nm);
            }
        }
    }

    /* Small neon signs in the windows of the lower floors (bars, clinics, noodle joints). */
    int floors = (int)minf(5.0f, (top - (gy + gh)) / fh - 1.0f);
    for (int fl = 0; fl < floors; ++fl) {
        for (float u = 1.0f; u < f.width - 3.0f; u += 7.0f) {
            if (rng_float(r) > 0.16f) continue;
            float w = rng_range(r, 1.4f, 3.0f), h = rng_range(r, 0.55f, 1.0f);
            float y0 = gy + gh + fl * fh + 1.1f;
            uint8_t col = (uint8_t)(rng_u32(r) & 7);
            uint8_t flick = (uint8_t)(rng_float(r) < 0.2f ? 1 : 0);
            emit_face_panel(m, &f, u, u + w, y0, y0 + h, 0.1f,
                            mat(MAT_NEON, rng_u32(r), (uint8_t)(col | (1 << 3) | (flick << 6)), q_half(w), q_half(h)));
        }
    }
}

/* One large element on a tall section face: a giant video screen or a tall neon banner. */
static void face_feature(Mesh *m, const BGraph *g, const BSection *s, float width, Rng *r) {
    const float gy = SIDEWALK_HEIGHT;
    float th = s->y1 - s->y0;
    if (th < 30.0f || width < 10.0f) return;
    Face tf = face_local(width);
    float roll = rng_float(r);
    float tall = s->y1 - gy;
    if (tall > 70.0f && roll < 0.22f && width >= 14.0f) {
        float w = minf(width - 4.0f, rng_range(r, 14.0f, 42.0f));
        float h = rng_float(r) < 0.55f ? w * 0.5625f : w * 1.5f;
        h = minf(h, th - 6.0f);
        if (h < 8.0f) return;
        float u0 = rng_range(r, 2.0f, width - 2.0f - w);
        float y0 = s->y0 + rng_range(r, 0.3f, 0.9f) * (th - h - 2.0f) + 1.0f;
        emit_box(m, v3(u0 - 0.6f, y0 - 0.6f, 0.0f), v3(u0 + w + 0.6f, y0 + h + 0.6f, 0.45f), F_NOBOT | F_BOT, mat(MAT_METAL, g->seed, 3, 0, 0), UV_FACADE);
        emit_face_panel(m, &tf, u0, u0 + w, y0, y0 + h, 0.5f, mat(MAT_SCREEN, rng_u32(r), 0, q_half(w), q_half(h)));
    } else if (roll < 0.42f && g->archetype != ARCH_ARCOLOGY) {
        float w = rng_range(r, 3.0f, 6.0f);
        float h = minf(rng_range(r, 12.0f, 45.0f), th * 0.7f);
        float u0 = rng_range(r, 1.0f, width - 1.0f - w);
        float y0 = s->y0 + rng_range(r, 0.1f, 0.9f) * (th - h - 1.0f) + 0.5f;
        if (s->y0 <= gy + 0.01f) y0 = maxf(y0, gy + g->groundH + 1.0f);
        if (y0 + h > s->y1 - 0.5f) return;
        uint8_t col = (uint8_t)(rng_u32(r) & 7);
        emit_face_panel(m, &tf, u0, u0 + w, y0, y0 + h, 0.3f, mat(MAT_NEON, rng_u32(r), (uint8_t)(col | (2 << 3)), q_half(w), q_half(h)));
    }
}

static void emit_doors(Mesh *m, const BGraph *g) {
    const float gy = SIDEWALK_HEIGHT;
    Mat metal = mat(MAT_METAL, g->seed, 3, 0, 0);
    for (int d = 0; d < g->ndoor; ++d) {
        const BDoor *dr = &g->door[d];
        Xf sv = xf_save(m);
        float len = enter_face(m, &g->sec[dr->section].foot.outer, dr->face);
        Face f = face_local(len);
        float u0 = dr->u - dr->width * 0.5f, u1 = dr->u + dr->width * 0.5f, y1 = gy + dr->height;
        emit_box(m, v3(u0 - 0.15f, gy, 0.0f), v3(u1 + 0.15f, y1 + 0.15f, 0.08f), F_NOBOT, metal, UV_FACADE);
        emit_face_panel(m, &f, u0, u1, gy, y1, 0.1f, mat(MAT_DOOR, hash_combine(g->seed, (uint32_t)d), (uint8_t)dr->type, q_half(dr->width), q_half(dr->height)));
        if (dr->canopy) {
            emit_box(m, v3(u0 - 0.8f, y1 + 0.3f, 0.0f), v3(u1 + 0.8f, y1 + 0.55f, 1.8f), F_NOBOT | F_BOT, metal, UV_FACADE);
            float uv[4][2] = { { 0, 0 }, { u1 - u0 + 1.4f, 0 }, { u1 - u0 + 1.4f, 1 }, { 0, 1 } };
            emit_quad(m, v3(u0 - 0.7f, y1 + 0.29f, 1.55f), v3(u1 + 0.7f, y1 + 0.29f, 1.55f), v3(u1 + 0.7f, y1 + 0.29f, 1.75f),
                      v3(u0 - 0.7f, y1 + 0.29f, 1.75f), v3(0, -1, 0), uv, mat(MAT_LEDSTRIP, g->seed, (uint8_t)(g->seed & 7), 0, 0));
        } else if (dr->type != DOOR_PRIMARY) {
            emit_emissive_box(m, v3(dr->u, y1 + 0.45f, 0.25f), 0.15f, 0, 2, 0);   /* a sodium bulb over the door */
        }
        xf_restore(m, sv);
    }
}

/* Everything that makes up one building, in its own frame. */
static void emit_building(Mesh *m, const Building *b, const BGraph *g) {
    const float gy = SIDEWALK_HEIGHT;
    Rng r = rng_make(b->seed ^ 0x77u);
    int fins = 0;
    float bay = bgen_bay_width(g->seed, g->fine);
    for (int si = 0; si < g->nsec; ++si) {
        const BSection *s = &g->sec[si];
        emit_walls(m, b, g, si);
        emit_cap(m, &s->foot, s->y1, 0, mat(MAT_ROOF, g->seed, (uint8_t)si, 0, 0));
        if (s->y0 > gy + 0.01f) emit_cap(m, &s->foot, s->y0, 1, mat(MAT_ROOF, g->seed, (uint8_t)si, 1, 0));
        const BLoop *l = &s->foot.outer;
        uint8_t ledCol = (uint8_t)(hash_combine(g->seed, (uint32_t)si) & 7);
        for (int k = 0; k < l->n; ++k) {
            Xf sv = xf_save(m);
            float len = enter_face(m, l, k);
            Face f = face_local(len);
            /* LED strip under the top edge, and an accent band where a tower meets its base */
            if (s->ledTop && s->roof != ROOF_HIP && s->roof != ROOF_PYRAMID) {
                float uv[4][2] = { { 0, 0 }, { len, 0 }, { len, 1 }, { 0, 1 } };
                emit_quad(m, v3(0, s->y1 - 0.75f, 0.06f), v3(len, s->y1 - 0.75f, 0.06f), v3(len, s->y1 - 0.45f, 0.06f),
                          v3(0, s->y1 - 0.45f, 0.06f), v3(0, 0, 1), uv, mat(MAT_LEDSTRIP, g->seed, ledCol, (uint8_t)(ledCol & 1), 0));
            }
            if (s->accent) {
                emit_box(m, v3(0, s->y0, 0), v3(len, s->y0 + 0.9f, 0.3f), F_NOBOT | F_BOT, mat(MAT_METAL, g->seed, 3, 0, 0), UV_FACADE);
                float uv[4][2] = { { 0, 0 }, { len, 0 }, { len, 1 }, { 0, 1 } };
                emit_quad(m, v3(0, s->y0 + 0.35f, 0.31f), v3(len, s->y0 + 0.35f, 0.31f), v3(len, s->y0 + 0.55f, 0.31f),
                          v3(0, s->y0 + 0.55f, 0.31f), v3(0, 0, 1), uv, mat(MAT_LEDSTRIP, g->seed, (uint8_t)(ledCol + 3), 1, 0));
            }
            /* vertical fins on the bay lines (the shader stretches bays to fit the face) */
            if (s->fins && fins < 320) {
                int nB = (int)maxf(1.0f, floorf(len / bay + 0.5f));
                float ys = s->y0 <= gy + 0.01f ? gy + g->groundH : s->y0;
                for (int j = 1; j < nB && fins < 320; ++j, ++fins) {
                    float u = (float)j * len / (float)nB;
                    emit_box(m, v3(u - 0.09f, ys, 0.0f), v3(u + 0.09f, s->y1, 0.45f), F_NOBOT | F_BOT, mat(MAT_METAL, g->seed, 3, 0, 0), UV_FACADE);
                }
            }
            /* parapet where the roof edge is open */
            if (s->roof != ROOF_HIP && s->roof != ROOF_PYRAMID) {
                EdgeInfo e = edge_of(l, k);
                float mx = e.ax + e.ex * len * 0.5f + e.ez * 0.6f, mz = e.az + e.ez * len * 0.5f - e.ex * 0.6f;
                if (roof_free(g, si, mx, mz, 0.0f))
                    emit_box(m, v3(0, s->y1, -0.35f), v3(len, s->y1 + 1.1f, 0), F_NOBOT, mat(MAT_METAL, g->seed, 0, 0, 0), UV_FACADE);
            }
            (void)f;
            xf_restore(m, sv);
        }
        dress_roof(m, g, si, &r);

        /* masts, spires and aircraft warning lights on the tops */
        if (!has_above(g, si)) {
            float cx = 0, cz = 0;
            for (int k = 0; k < l->n; ++k) { cx += l->x[k]; cz += l->z[k]; }
            cx /= (float)l->n; cz /= (float)l->n;
            float tall = s->y1 - gy;
            float mh = s->spire ? (float)s->spire : (tall > 60.0f && rng_float(&r) < 0.5f ? rng_range(&r, 8.0f, 28.0f) : 0.0f);
            if (mh > 0.0f && bfoot_contains(&s->foot, cx, cz) && s->roof != ROOF_HELIPAD && s->roof != ROOF_PYRAMID) {
                float w = g->archetype == ARCH_ARCOLOGY ? 1.2f : s->spire ? 0.5f : 0.35f;
                emit_box(m, v3(cx - w, s->y1, cz - w), v3(cx + w, s->y1 + mh, cz + w), F_NOBOT, mat(MAT_METAL, g->seed, 2, 0, 0), UV_FACADE);
                emit_emissive_box(m, v3(cx, s->y1 + mh + 0.4f, cz), 0.4f, 2, 1, (uint8_t)(rng_u32(&r) & 255));
            }
            if (tall > 90.0f) {
                uint8_t ph = (uint8_t)(rng_u32(&r) & 255);
                for (int k = 0; k < l->n; ++k) {
                    FaceKey fk = face_key(l, k, 0);
                    if (!fk.startConvex) continue;
                    float vx = l->x[k] + (cx - l->x[k]) * 0.4f / maxf(1.0f, sqrtf((cx - l->x[k]) * (cx - l->x[k]) + (cz - l->z[k]) * (cz - l->z[k])));
                    float vz = l->z[k] + (cz - l->z[k]) * 0.4f / maxf(1.0f, sqrtf((cx - l->x[k]) * (cx - l->x[k]) + (cz - l->z[k]) * (cz - l->z[k])));
                    emit_emissive_box(m, v3(vx, s->y1 + 1.5f, vz), 0.3f, 2, 1, ph);
                }
            }
        }
    }

    /* supports */
    Mat concrete = mat(MAT_METAL, g->seed, 5, 0, 0);
    for (int i = 0; i < g->nsup; ++i) {
        const BSupport *s = &g->sup[i];
        if (s->type == SUP_COLUMN) {
            float h = s->size * 0.5f;
            emit_box(m, v3(s->x0 - h, s->y0, s->z0 - h), v3(s->x0 + h, s->y1, s->z0 + h), F_SIDES, concrete, UV_FACADE);
        } else if (s->type == SUP_STRUT) {
            emit_beam(m, v3(s->x0, s->y0, s->z0), v3(s->x1, s->y1, s->z1), s->size, concrete);
        } else {
            emit_box(m, v3(s->x0, s->y0, s->z0), v3(s->x1, s->y1, s->z1), F_SIDES | F_BOT, concrete, UV_FACADE);
        }
    }
    emit_doors(m, g);

    /* signs: street level on faces onto the street, big elements on tall faces */
    for (int si = 0; si < g->nsec; ++si) {
        const BSection *s = &g->sec[si];
        const BLoop *l = &s->foot.outer;
        for (int k = 0; k < l->n; ++k) {
            EdgeInfo e = edge_of(l, k);
            if (!face_toward(b, &e)) continue;
            FaceKey fk = face_key(l, k, 0);
            Xf sv = xf_save(m);
            enter_face(m, l, k);
            if (s->y0 <= gy + 0.01f) {
                float px = e.ax + e.ex * e.len * 0.5f + e.ez * 0.3f, pz = e.az + e.ez * e.len * 0.5f - e.ex * 0.3f;
                face_signs(m, b, g, si, &fk, facade_top(g, si, px, pz), &r);
            }
            face_feature(m, g, s, e.len, &r);
            xf_restore(m, sv);
        }
    }

    /* Rooftop billboard on low buildings, facing a street. */
    for (int si = 0; si < g->nsec; ++si) {
        const BSection *s = &g->sec[si];
        if (has_above(g, si) || s->y1 - gy > 70.0f || s->roof == ROOF_HIP || s->roof == ROOF_PYRAMID || s->roof == ROOF_HELIPAD) continue;
        if (g->archetype != ARCH_OFFICE && g->archetype != ARCH_SHOPHOUSE && g->archetype != ARCH_SHED && g->archetype != ARCH_SLAB) continue;
        if (rng_float(&r) >= 0.3f) continue;
        const BLoop *l = &s->foot.outer;
        int best = -1;
        float bl = 0.0f;
        for (int k = 0; k < l->n; ++k) {
            EdgeInfo e = edge_of(l, k);
            if (face_toward(b, &e) && e.len > bl) { bl = e.len; best = k; }
        }
        if (best < 0 || bl < 8.0f) continue;
        Xf sv = xf_save(m);
        enter_face(m, l, best);
        Face f = face_local(bl);
        float w = minf(f.width - 2.0f, rng_range(&r, 8.0f, 18.0f));
        float h = minf(w * 0.42f, 8.0f);
        float u0 = (f.width - w) * 0.5f;
        float yb = s->y1 + rng_range(&r, 2.0f, 4.0f);
        Mat metal = mat(MAT_METAL, b->seed, 3, 0, 0);
        for (int leg = 0; leg < 3; ++leg) {
            float uu = u0 + w * (0.15f + 0.35f * leg);
            emit_box(m, v3(uu - 0.15f, s->y1, -1.6f), v3(uu + 0.15f, yb, -1.3f), F_NOBOT, metal, UV_FACADE);
        }
        emit_box(m, v3(u0 - 0.3f, yb, -1.7f), v3(u0 + w + 0.3f, yb + h + 0.3f, -1.25f), F_NOBOT | F_BOT, metal, UV_FACADE);
        if (rng_float(&r) < 0.4f)
            emit_face_panel(m, &f, u0, u0 + w, yb + 0.15f, yb + h + 0.15f, -1.2f, mat(MAT_SCREEN, rng_u32(&r), 0, q_half(w), q_half(h)));
        else
            emit_face_panel(m, &f, u0, u0 + w, yb + 0.15f, yb + h + 0.15f, -1.2f,
                            mat(MAT_NEON, rng_u32(&r), (uint8_t)((rng_u32(&r) & 7) | (1 << 3)), q_half(w), q_half(h)));
        xf_restore(m, sv);
        break;
    }
}

/* ===========================================================================
 * Streets
 * ========================================================================= */

#define BASE_Y (-0.03f)   /* plain asphalt under everything; marked carriageways sit at 0 */

/* Now and then a street lamp is failing (lampState in shaders/common.glsl):
 * about one in 250 is dead, one in 250 cycles on and off as a worn sodium
 * lamp does, and one in 250 burns dim and red. State in the low 2 bits, a
 * seed for its rhythm above; the same for every chunk that sees the lamp. */
static uint32_t lamp_fault(const Lamp *L) {
    uint32_t h = hash2i(WORLD_SEED ^ 0x1A3BADu, (int32_t)floor(L->x * 4.0), (int32_t)floor(L->z * 4.0));
    float r = hash_float(h);
    uint32_t state = r < 0.004f ? 1u : r < 0.008f ? 2u : r < 0.012f ? 3u : 0u;
    return state | (hash_u32(h) & 0x3FFFFFu) << 2;
}

/* Pole at the origin, arm along +x (the emission transform turns it to the road). */
static void build_lamp(Mesh *m, const ChunkGen *g, const Lamp *L, uint32_t idx) {
    m->xf = 1;
    m->xc = L->dx; m->xs = L->dz;
    m->xx = (float)(L->x - g->ox); m->xz = (float)(L->z - g->oz);
    const float gy = SIDEWALK_HEIGHT, top = LAMP_HEIGHT;
    Mat metal = mat(MAT_METAL, idx, 4, 0, 0);
    emit_box(m, v3(-0.11f, gy, -0.11f), v3(0.11f, top, 0.11f), F_NOBOT, metal, UV_FACADE);
    emit_box(m, v3(-0.2f, gy, -0.2f), v3(0.2f, gy + 1.0f, 0.2f), F_NOBOT, metal, UV_FACADE);
    emit_box(m, v3(-0.06f, top - 0.14f, -0.06f), v3(1.96f, top, 0.06f), F_NOBOT | F_BOT, metal, UV_FACADE);
    /* head: 1.7 m out, over the edge of the carriageway */
    const float hx = 1.7f, hw = 0.42f, hd = 0.2f;
    emit_box(m, v3(hx - hw, top - 0.32f, -hd), v3(hx + hw, top - 0.05f, hd), F_NOBOT, metal, UV_FACADE);
    float uv[4][2] = { {0,0}, {1,0}, {1,1}, {0,1} };
    float y = top - 0.33f;
    emit_quad(m, v3(hx - hw + 0.04f, y, -hd + 0.04f), v3(hx + hw - 0.04f, y, -hd + 0.04f),
              v3(hx + hw - 0.04f, y, hd - 0.04f), v3(hx - hw + 0.04f, y, hd - 0.04f),
              v3(0, -1, 0), uv, mat(MAT_EMISSIVE, lamp_fault(L), 0, (uint8_t)(2 + L->type), 0));
    m->xf = 0;
}

static int clip_axis(const DVec2 *in, int n, int axis, double sign, double lim, DVec2 *out) {
    int k = 0;
    for (int i = 0; i < n; ++i) {
        DVec2 a = in[i], b = in[(i + 1) % n];
        double da = sign * ((axis ? a.z : a.x) - lim), db = sign * ((axis ? b.z : b.x) - lim);
        if (da <= 0.0) out[k++] = a;
        if ((da <= 0.0) != (db <= 0.0)) {
            double t = da / (da - db);
            out[k++] = dv(a.x + (b.x - a.x) * t, a.z + (b.z - a.z) * t);
        }
    }
    return k;
}

/* Clip a convex polygon (chunk-local) to the chunk square. */
static int clip_chunk(const DVec2 *in, int n, DVec2 *out) {
    const double P = CHUNK_SIZE;
    DVec2 a[48], b[48];
    if (n > 40) n = 40;
    n = clip_axis(in, n, 0, -1.0, 0.0, a);
    n = clip_axis(a, n, 0, 1.0, P, b);
    n = clip_axis(b, n, 1, -1.0, 0.0, a);
    n = clip_axis(a, n, 1, 1.0, P, out);
    return n >= 3 ? n : 0;
}

static int clip_segment(DVec2 *a, DVec2 *b) {
    const double P = CHUNK_SIZE;
    double t0 = 0.0, t1 = 1.0, dx = b->x - a->x, dz = b->z - a->z;
    double p[4] = { -dx, dx, -dz, dz }, q[4] = { a->x, P - a->x, a->z, P - a->z };
    for (int k = 0; k < 4; ++k) {
        if (fabs(p[k]) < 1e-12) { if (q[k] < 0.0) return 0; continue; }
        double r = q[k] / p[k];
        if (p[k] < 0.0) t0 = fmax(t0, r); else t1 = fmin(t1, r);
    }
    if (t1 - t0 < 1e-6) return 0;
    DVec2 a0 = *a;
    *a = dv(a0.x + dx * t0, a0.z + dz * t0);
    *b = dv(a0.x + dx * t1, a0.z + dz * t1);
    return 1;
}

/* Carriageways, cut back at junctions so markings never overlap. uv = (offset
 * from the centre line, distance along); seed carries the length for the
 * crossings at each end, params the half-width (quarter metres) and class. */
static void build_roads(Mesh *m, const ChunkGen *g) {
    const double P = CHUNK_SIZE;
    DVec2 base[4] = { {0, 0}, {P, 0}, {P, P}, {0, P} };
    float buv[4][2] = { {0, 0}, {0, 0}, {0, 0}, {0, 0} };
    emit_flat(m, base, 4, BASE_Y, buv, mat(MAT_ROAD, 0, 0, 0, 0));

    for (int ri = 0; ri < g->pa->nroads; ++ri) {
        const Road *r = &g->pa->roads[ri];
        DVec2 a = dv(r->a.x - g->ox, r->a.z - g->oz), b = dv(r->b.x - g->ox, r->b.z - g->oz);
        double len = dlen(dsub(b, a));
        double L = len - r->trimA - r->trimB;
        if (len < 1.0 || L < 1.0) continue;
        DVec2 dir = dscale(dsub(b, a), 1.0 / len), nrm = dv(-dir.z, dir.x);
        DVec2 s0 = dadd(a, dscale(dir, r->trimA)), s1 = dadd(a, dscale(dir, len - r->trimB));
        double h = r->at.half;
        DVec2 q[4] = { dadd(s0, dscale(nrm, h)), dadd(s1, dscale(nrm, h)), dsub(s1, dscale(nrm, h)), dsub(s0, dscale(nrm, h)) };
        DVec2 c[48];
        int n = clip_chunk(q, 4, c);
        if (!n) continue;
        float uv[48][2];
        for (int i = 0; i < n; ++i) {
            DVec2 d = dsub(c[i], s0);
            uv[i][0] = (float)ddot(d, nrm);
            uv[i][1] = (float)ddot(d, dir);
        }
        float Lf = (float)L;
        uint32_t lbits;
        memcpy(&lbits, &Lf, 4);
        uint8_t hq = (uint8_t)clampf(r->at.half * 4.0f + 0.5f, 1.0f, 255.0f);
        emit_flat(m, c, n, 0.0f, (const float (*)[2])uv, mat(MAT_ROAD, lbits, hq, (uint8_t)(L > 16.0 ? 1 : 0), r->at.cls));
    }
}

/* Raised blocks: sidewalk, kerb stones, kerb faces and the lot inside. */
static void build_blocks(Mesh *m, const ChunkGen *g) {
    const double P = CHUNK_SIZE;
    const float gy = SIDEWALK_HEIGHT;
    double wx = (g->cx & 63) * P, wz = (g->cz & 63) * P;   /* wrapped offset, as gWorld in the shader */
    for (int bi = 0; bi < g->pa->nblocks; ++bi) {
        const Block *bk = &g->pa->blocks[bi];
        if (bk->x1 < g->ox || bk->x0 > g->ox + P || bk->z1 < g->oz || bk->z0 > g->oz + P) continue;
        int n = bk->curb.n;
        DVec2 c[PLAN_MAX_PV], in[PLAN_MAX_PV], cen = dv(0, 0);
        for (int i = 0; i < n; ++i) {
            c[i] = dv(bk->curb.v[i].x - g->ox, bk->curb.v[i].z - g->oz);
            cen = dadd(cen, c[i]);
        }
        cen = dscale(cen, 1.0 / n);
        /* Inward edge normals and mitred inner corners for the kerb-stone strip. */
        DVec2 nin[PLAN_MAX_PV];
        for (int i = 0; i < n; ++i) {
            DVec2 d = dsub(c[(i + 1) % n], c[i]);
            nin[i] = dlen(d) > 1e-6 ? dnorm(dv(-d.z, d.x)) : (i ? nin[i - 1] : dv(0, 0));
            if (ddot(nin[i], dsub(cen, c[i])) < 0) nin[i] = dv(-nin[i].x, -nin[i].z);
        }
        for (int i = 0; i < n; ++i) {
            DVec2 n0 = nin[(i + n - 1) % n], n1 = nin[i];
            DVec2 off = dscale(dadd(n0, n1), KERB_STONE_WIDTH / fmax(1.0 + ddot(n0, n1), 0.05));
            if (dlen(off) > 1.2) off = dscale(dnorm(off), 1.2);
            in[i] = dadd(c[i], off);
        }
        float gc = bk->gc, gs = bk->gs;
        DVec2 cl[48];
        float uv[48][2];
#define PAVE_UV(k) do { double X = cl[k].x + wx, Z = cl[k].z + wz; \
        uv[k][0] = (float)(gc * X + gs * Z); uv[k][1] = (float)(-gs * X + gc * Z); } while (0)
        /* The seed carries the grain angle (float bits): the tangent frame for normal-mapped paving. */
        float grain = (float)atan2(gs, gc);
        uint32_t gbits;
        memcpy(&gbits, &grain, 4);

        int k = clip_chunk(c, n, cl);
        for (int i = 0; i < k; ++i) PAVE_UV(i);
        if (k) emit_flat(m, cl, k, gy, (const float (*)[2])uv, mat(MAT_SIDEWALK, gbits, 0, 0, 0));

        for (int i = 0; i < n; ++i) {
            int j = (i + 1) % n;
            DVec2 strip[4] = { c[i], c[j], in[j], in[i] };
            k = clip_chunk(strip, 4, cl);
            /* Kerb stones are laid along the edge: u runs along it and v inward from the
             * outer edge (metres), so the shader knows where the joint with the paving
             * falls (v = KERB_STONE_WIDTH). The inward normal is u turned 90 degrees,
             * (-e.z, e.x), so the edge angle in the seed rebuilds the whole frame. */
            DVec2 along = dv(nin[i].z, -nin[i].x);
            float ea = (float)atan2(along.z, along.x);
            uint32_t ebits;
            memcpy(&ebits, &ea, 4);
            for (int t = 0; t < k; ++t) {
                DVec2 d = dsub(cl[t], c[i]);
                uv[t][0] = (float)ddot(d, along);
                uv[t][1] = (float)ddot(d, nin[i]);
            }
            if (k) emit_flat(m, cl, k, gy + 0.003f, (const float (*)[2])uv, mat(MAT_SIDEWALK, ebits, 1, 0, 0));

            DVec2 a = c[i], b = c[j];
            if (!clip_segment(&a, &b)) continue;
            vec3 nn = v3((float)-nin[i].x, 0, (float)-nin[i].z);
            float u0 = (float)dlen(dsub(a, c[i])), u1 = (float)dlen(dsub(b, c[i]));
            /* u runs along cross(up, n), so the shader can rebuild the face's tangent from its normal. */
            DVec2 e = dsub(c[j], c[i]);
            if (e.x * nn.z - e.z * nn.x < 0) { u0 = -u0; u1 = -u1; }
            float quv[4][2] = { { u0, BASE_Y }, { u1, BASE_Y }, { u1, gy }, { u0, gy } };
            emit_quad(m, v3((float)a.x, BASE_Y, (float)a.z), v3((float)b.x, BASE_Y, (float)b.z),
                      v3((float)b.x, gy, (float)b.z), v3((float)a.x, gy, (float)a.z), nn, quv, mat(MAT_CURB, 0, 0, 0, 0));
        }

        if (bk->lot.n) {
            DVec2 l[PLAN_MAX_PV];
            for (int i = 0; i < bk->lot.n; ++i) l[i] = dv(bk->lot.v[i].x - g->ox, bk->lot.v[i].z - g->oz);
            k = clip_chunk(l, bk->lot.n, cl);
            for (int i = 0; i < k; ++i) PAVE_UV(i);
            if (k) emit_flat(m, cl, k, gy + 0.006f, (const float (*)[2])uv, mat(MAT_LOT, 0, (uint8_t)(bk->zone == ZONE_PARK), 0, 0));
        }
#undef PAVE_UV
    }
}

/* ===========================================================================
 * Light tiles
 *
 * Street lamps and lit shopfronts near the chunk are packed into a small
 * table the fragment shader walks instead of evaluating every light: the
 * chunk's mesh bounds are cut into 4x4 tiles, each listing the lights that
 * can reach it. Record layout (4 x uint32 each):
 *   0      bounds x0, z0 (chunk-local) and 4/width, 4/depth (floats)
 *   1..4   16 tile words: first record << 16 | count
 *   5..    lights: x, y, z (floats), w = type | data
 *          type 0/1: sodium/LED street lamp; type 2: shop (w & ~3 = shop hash);
 *          type 3: office or estate lobby (white)
 * ========================================================================= */

typedef struct LightSrc { float x, y, z, range; uint32_t w; } LightSrc;

/* A lamp or shopfront lighting the rain mist, facing (nx, nz) if it has a
 * front. Several chunks see the same light; only the chunk it stands in
 * records it. */
static void air_light(Mesh *m, float x, float y, float z, float power, float nx, float nz, uint32_t seed, uint32_t kind) {
    if (x < 0.0f || x >= CHUNK_SIZE || z < 0.0f || z >= CHUNK_SIZE) return;
    SkyLight L = { { x, y, z }, power, nx, nz, seed, kind };
    push_light(&m->air, &m->nair, &m->capair, &L);
}

static void build_lights(Mesh *m, ChunkGen *g) {
    if (!m->count) return;
    float bx0 = m->mn[0], bz0 = m->mn[2], bx1 = m->mx[0], bz1 = m->mx[2];
    int cap = 1024, ns = 0;
    LightSrc *src = (LightSrc *)malloc(sizeof(LightSrc) * cap);
    if (!src) fatal("out of memory building chunk lights");
#define ADD_LIGHT(X, Y, Z, R, W) do { if (ns == cap) { cap *= 2; src = (LightSrc *)realloc(src, sizeof(LightSrc) * cap); \
        if (!src) fatal("out of memory building chunk lights"); } \
        src[ns].x = (X); src[ns].y = (Y); src[ns].z = (Z); src[ns].range = (R); src[ns].w = (W); ns++; } while (0)

    for (int i = 0; i < g->nl; ++i) {
        const Lamp *L = &g->lamp[i];
        float x = (float)(L->x - g->ox) + L->dx * 1.7f, z = (float)(L->z - g->oz) + L->dz * 1.7f;
        if (x < bx0 - 34.0f || x > bx1 + 34.0f || z < bz0 - 34.0f || z > bz1 + 34.0f) continue;
        uint32_t fault = lamp_fault(L);
        if ((fault & 3u) == 1u) continue;   /* dead: lights nothing */
        ADD_LIGHT(x, LAMP_HEIGHT - 0.4f, z, 34.0f, (uint32_t)L->type | fault << 2);
        air_light(m, x, LAMP_HEIGHT - 0.4f, z, 70.0f, 0.0f, 0.0f, fault, (L->type ? AIR_LAMP_LED : AIR_LAMP_SODIUM) << 8);
    }
    for (int i = 0; i < g->nb; ++i) {
        const Building *b = &g->bld[i];
        /* plots wholly out of reach can be skipped without generating them */
        double rr = 0.5 * sqrt((double)(b->foot.x1 - b->foot.x0) * (b->foot.x1 - b->foot.x0) + (double)(b->foot.z1 - b->foot.z0) * (b->foot.z1 - b->foot.z0)) + 14.0;
        float lx = (float)(b->ox - g->ox), lz = (float)(b->oz - g->oz);
        if (lx < bx0 - rr || lx > bx1 + rr || lz < bz0 - rr || lz > bz1 + rr) continue;
        const BGraph *bg = gen_graph(g, i);
        int retail = bg->groundMode == GF_RETAIL;
        if (retail && !b->shop) continue;
        for (int si = 0; si < bg->nsec; ++si) {
            const BSection *s = &bg->sec[si];
            if (s->y0 > SIDEWALK_HEIGHT + 0.01f) continue;
            const BLoop *l = &s->foot.outer;
            for (int k = 0; k < l->n; ++k) {
                FaceKey fk = face_key(l, k, 0);
                EdgeInfo e = edge_of(l, k);
                float step = retail ? 6.0f : 8.0f;
                for (int seg = 0; seg * step < e.len; ++seg) {
                    float u0 = seg * step, u1 = minf(u0 + step, e.len);
                    if (u1 - u0 < 2.0f) continue;
                    uint32_t h = 3u;                               /* lobby: cool white */
                    if (retail) {
                        h = city_shop_hash(b->seed, si, fk.face, seg, e.len, fk.next, fk.endConvex);
                        if (!city_shop_lit(h, b->shop)) continue;
                        h = (h & ~3u) | 2u;
                    }
                    float u = (u0 + u1) * 0.5f;
                    DVec2 w = to_world(b, e.ax + e.ex * u - e.ez * 1.0f, e.az + e.ez * u + e.ex * 1.0f);
                    float x = (float)(w.x - g->ox), z = (float)(w.z - g->oz);
                    if (x < bx0 - 12.0f || x > bx1 + 12.0f || z < bz0 - 12.0f || z > bz1 + 12.0f) continue;
                    int party = 0;   /* a face against a neighbour is not a shopfront */
                    for (int j = 0; j < g->nb && !party; ++j)
                        if (j != i && g->bblk[j] == g->bblk[i] && inside_building(&g->bld[j], w.x, w.z, 0.0)) party = 1;
                    if (party) continue;
                    ADD_LIGHT(x, SIDEWALK_HEIGHT + 1.4f, z, 12.0f, h);
                    float nx = b->c * -e.ez - b->s * e.ex, nz = b->s * -e.ez + b->c * e.ex;   /* out of the shopfront */
                    air_light(m, x, SIDEWALK_HEIGHT + 2.0f, z, 5.0f, nx, nz, h, ((h & 3u) == 2u ? AIR_SHOP : AIR_LOBBY) << 8);
                }
            }
        }
    }
#undef ADD_LIGHT

    uint32_t *rec = (uint32_t *)calloc(LIGHT_PAGE_RECS * 4, sizeof(uint32_t));
    if (!rec) fatal("out of memory building chunk lights");
    float sx = maxf((bx1 - bx0) * 0.25f, 0.25f), sz = maxf((bz1 - bz0) * 0.25f, 0.25f);
    float hdr[4] = { bx0, bz0, 1.0f / sx, 1.0f / sz };
    memcpy(rec, hdr, sizeof(hdr));
    uint32_t next = 5;
    for (int t = 0; t < 16; ++t) {
        float tx0 = bx0 + (t & 3) * sx, tz0 = bz0 + (t >> 2) * sz, tx1 = tx0 + sx, tz1 = tz0 + sz;
        uint32_t start = next, count = 0;
        for (int i = 0; i < ns && next < LIGHT_PAGE_RECS; ++i) {
            float dx = maxf(maxf(tx0 - src[i].x, src[i].x - tx1), 0.0f);
            float dz = maxf(maxf(tz0 - src[i].z, src[i].z - tz1), 0.0f);
            if (dx * dx + dz * dz > src[i].range * src[i].range) continue;
            uint32_t *o = &rec[next * 4];
            memcpy(&o[0], &src[i].x, 4); memcpy(&o[1], &src[i].y, 4); memcpy(&o[2], &src[i].z, 4);
            o[3] = src[i].w;
            next++; count++;
        }
        rec[4 + t] = (start << 16) | count;
    }
    free(src);
    m->lights = rec;
    m->nlights = next;
}

/* ===========================================================================
 * Overhead cables: power and data lines strung across streets from facade to
 * facade (singly or in loose bundles, now and then high between towers), from
 * lamp to lamp along the sidewalk, from lamps up to the wall behind them, and
 * criss-crossing the alleys and courtyards between buildings.
 *
 * Street stations are spaced along each road by a stream that depends on the
 * road alone, alley stations along each wall by a stream that depends on that
 * wall alone, and each station rolls its own choices from its own hash, so a
 * cable comes out the same from whichever chunk it is generated. A crossing
 * belongs to the chunk holding its station on the centre line, a lamp line to
 * the chunk holding its first lamp, an alley cable to its first building's.
 *
 * Ends are found by casting horizontally into the building graphs, so every
 * cable ends on a wall, and never on glass: an end slides along the wall it
 * hit to the nearest spot the facade shader draws solid (the band between two
 * rows of windows, the pier between two bays, the pier between two shopfronts
 * or a lobby mullion), clear of door canopies and the neon in the windows.
 * ========================================================================= */

#define CABLE_CLEAR 5.0f  /* lowest a crossing sags: clear of trucks (and of lamps, checked apart) */
#define ALLEY_CLEAR 3.4f   /* lowest an alley cable sags: clear of heads */
#define STREET_GAP  0.12f  /* solid wall needed round an end over a street (the bracket and a little)... */
#define ALLEY_GAP_U 0.3f   /* ...and in an alley, either side, where a skewed cable runs close along the wall... */
#define ALLEY_GAP_V 0.15f  /* ...and above and below, where it leaves the wall near level */

typedef struct Cable { vec3 a, b; float sag, thick; } Cable;
/* distance (< 0: none), building, section, loop (1: courtyard), edge, metres along the edge */
typedef struct WallHit { double t; int b, s, li, k; float u; } WallHit;

static int slab(double o, double u, double lo, double hi, double *t0, double *t1) {
    if (fabs(u) < 1e-12) return o >= lo && o <= hi;
    double a = (lo - o) / u, b = (hi - o) / u;
    if (a > b) { double t = a; a = b; b = t; }
    *t0 = fmax(*t0, a); *t1 = fmin(*t1, b);
    return *t0 <= *t1;
}

/* The nearest wall standing at height y along a horizontal ray from p (world,
 * unit dir), within maxT: outer walls and courtyard walls. */
static WallHit facade_ray(ChunkGen *g, DVec2 p, DVec2 dir, float y, double maxT) {
    WallHit h = { -1.0, -1, -1, 0, 0, 0.0f };
    double best = maxT;
    int back = 0;
    for (int i = 0; i < g->nb; ++i) {
        const Building *b = &g->bld[i];
        if (y > b->height + 30.0f) continue;
        double dx = p.x - b->ox, dz = p.z - b->oz;
        double lx = b->c * dx + b->s * dz, lz = -b->s * dx + b->c * dz;
        double ux = b->c * dir.x + b->s * dir.z, uz = -b->s * dir.x + b->c * dir.z;
        const double pad = 12.0;   /* projections reach past the plot */
        double t0 = 0.0, t1 = best;
        if (!slab(lx, ux, b->foot.x0 - pad, b->foot.x1 + pad, &t0, &t1) ||
            !slab(lz, uz, b->foot.z0 - pad, b->foot.z1 + pad, &t0, &t1)) continue;
        const BGraph *bg = gen_graph(g, i);
        for (int si = 0; si < bg->nsec; ++si) {
            const BSection *s = &bg->sec[si];
            if (y < s->y0 || y > s->y1) continue;
            const BLoop *loops[2] = { &s->foot.outer, &s->foot.hole };
            for (int li = 0; li < 2; ++li) {
                const BLoop *l = loops[li];
                for (int k = 0; k < l->n; ++k) {
                    int k1 = (k + 1) % l->n;
                    double ex = l->x[k1] - l->x[k], ez = l->z[k1] - l->z[k];
                    double den = ux * ez - uz * ex;
                    if (fabs(den) < 1e-9) continue;
                    double wx = l->x[k] - lx, wz = l->z[k] - lz;
                    double t = (wx * ez - wz * ex) / den, u = (uz * wx - ux * wz) / den;
                    if (u >= 0.0 && u <= 1.0 && t > 1e-3 && t < best) {
                        best = t; back = den < 0.0;
                        h.t = t; h.b = i; h.s = si; h.li = li; h.k = k; h.u = (float)(u * sqrt(ex * ex + ez * ez));
                    }
                }
            }
        }
    }
    /* outward normals are (-dz, dx): reaching a wall from behind means the ray started inside a building */
    if (back) h.t = -1.0;
    return h;
}

/* Door canopies stand out from the ground floor around each door. */
static int near_door(const BGraph *bg, int si, int li, int k, float u, float y) {
    if (li) return 0;
    for (int d = 0; d < bg->ndoor; ++d) {
        const BDoor *dr = &bg->door[d];
        if (dr->section == si && dr->face == k && fabsf(u - dr->u) < dr->width * 0.5f + 1.2f && y < SIDEWALK_HEIGHT + dr->height + 1.0f)
            return 1;
    }
    return 0;
}

/* The solid spot nearest (u, y) on face k of section si (u in metres along
 * the face, as the shader's uv.x), within 2.5 m, with at least mu of solid wall
 * either side of it and mv above and below, by the facade rules of
 * shadeFacade in shaders/city.frag:
 *   ground floor   glass between the piers of 6 m shop bays (1.5 m lobby
 *                  modules, whose glass stops short of the floor above);
 *   upper floors   a window per bay per floor: the band between two rows of
 *                  windows and the pier between two bays are solid (a
 *                  curtain wall's spandrels and mullions included), and so
 *                  are plant floors.
 * Black- and blue-glass cladding with windows punched in it is glass all over
 * above the ground floor; as a curtain wall, its grid still counts.
 * Shop boards stop 0.4 m short of the shop piers; the neon signs in the
 * windows of the lower floors (face_signs) are kept clear of. */
/* One face's window rhythm, as shadeFacade lays it out. */
typedef struct Rhythm {
    float winW, winH, sill;   /* window share of a bay and of a floor; sill height share */
    float base, fh, bw;       /* first floor's height, floor height, bay width (stretched to whole bays) */
    float glassTop;           /* ground floor: top of the shop or lobby glass */
    float step, edge;         /* ground floor: shop bay or lobby module, and its solid margin each side */
    int   atGround, cladding; /* the section stands on the ground; glass cladding with punched windows */
} Rhythm;

static Rhythm rhythm(const BGraph *bg, int si, float faceW) {
    const BSection *s = &bg->sec[si];
    uint32_t sh = style_hash(bg->seed);
    float h4 = hash_float(hash_combine(sh, 4u)), h5 = hash_float(hash_combine(sh, 5u)), h9 = hash_float(hash_combine(sh, 9u));
    Rhythm r;
    switch (s->win) {
    case WIN_CURTAIN: r.winW = 0.94f; r.winH = 0.86f; break;
    case WIN_RIBBON:  r.winW = 1.0f;  r.winH = lerpf(0.4f, 0.55f, h9); break;
    case WIN_SLIT:    r.winW = lerpf(0.2f, 0.32f, h5); r.winH = lerpf(0.7f, 0.85f, h9); break;
    default:          r.winW = lerpf(0.45f, 0.7f, h5); r.winH = lerpf(0.45f, 0.65f, h9); break;
    }
    r.sill = (1.0f - r.winH) * 0.42f;
    r.base = SIDEWALK_HEIGHT + bg->groundH;
    r.fh = bg->floorH;
    float bw0 = bg->fine ? 1.6f + 0.8f * h4 : 2.4f + 2.6f * h4;
    r.bw = faceW / maxf(1.0f, floorf(faceW / bw0 + 0.5f));
    int retail = bg->groundMode == GF_RETAIL;
    r.glassTop = r.base - (retail ? 1.5f : bg->groundMode == GF_DOUBLE ? 1.2f : 0.7f);
    r.step = retail ? 6.0f : 1.5f;
    r.edge = retail ? 0.3f : 0.06f;
    r.atGround = s->y0 <= SIDEWALK_HEIGHT + 0.01f;
    r.cladding = (s->palette == 4 || s->palette == 7) && s->win != WIN_CURTAIN;
    return r;
}

/* Any glass within mu along and mv up or down of (u, y) on a face of section
 * si? Off the section counts as glass: what is drawn there is another
 * section's business. */
static int glass_near(const BGraph *bg, int si, float faceW, float u, float y, float mu, float mv) {
    const BSection *s = &bg->sec[si];
    if (y - mv < s->y0 || y + mv > s->y1) return 1;
    Rhythm r = rhythm(bg, si, faceW);
    const float gy = SIDEWALK_HEIGHT;
    float u0 = u - mu, u1 = u + mu, y0 = y - mv, y1 = y + mv;
#define HITS(a0, a1, b0, b1) (u0 < (a1) && u1 > (a0) && y0 < (b1) && y1 > (b0))
    if (r.atGround && y0 < r.base) {
        float gb = gy + (r.step > 2.0f ? 0.45f : 0.08f);
        for (int j = (int)floorf(u0 / r.step); j <= (int)floorf(u1 / r.step); ++j)
            if (HITS((float)j * r.step + r.edge, (float)(j + 1) * r.step - r.edge, gb, r.glassTop)) return 1;
    }
    if (!r.atGround || y1 > r.base) {
        if (r.cladding) return 1;
        int every = 6 + 3 * (s->plant & 15);
        for (int k = (int)floorf((y0 - r.base) / r.fh); k <= (int)floorf((y1 - r.base) / r.fh); ++k) {
            if (r.atGround && k < 0) continue;
            if ((s->plant & 15) && k > 0 && k % every == every - 1) continue;   /* louvred plant floor */
            float b0 = r.base + r.fh * ((float)k + r.sill), b1 = b0 + r.fh * r.winH;
            for (int j = (int)floorf(u0 / r.bw); j <= (int)floorf(u1 / r.bw); ++j) {
                float a0 = r.bw * ((float)j + 0.5f - 0.5f * r.winW), a1 = r.bw * ((float)j + 0.5f + 0.5f * r.winW);
                if (HITS(a0, a1, b0, b1)) return 1;
            }
        }
    }
#undef HITS
    return 0;
}

static int solid_spot(const BGraph *bg, int si, int li, int k, float faceW, float u, float y, float mu, float mv, float *uo, float *yo) {
    const BSection *s = &bg->sec[si];
    Rhythm r = rhythm(bg, si, faceW);
    const float gy = SIDEWALK_HEIGHT;
    float winW = r.winW, winH = r.winH, sill = r.sill, base = r.base, fh = r.fh, bw = r.bw;
    int atGround = r.atGround;
    /* clear of the section's bottom edge, and of its top unless another section carries the face on up */
    const BLoop *l = li ? &s->foot.hole : &s->foot.outer;
    EdgeInfo e = edge_of(l, k);
    float px = e.ax + e.ex * clampf(u, 0.0f, faceW) + e.ez * 0.05f, pz = e.az + e.ez * clampf(u, 0.0f, faceW) - e.ex * 0.05f;   /* just inside */
    int carried = 0;
    for (int t = 0; t < bg->nsec && !carried; ++t)
        carried = t != si && fabsf(bg->sec[t].y0 - s->y1) < 0.05f && bfoot_contains(&bg->sec[t].foot, px, pz);
    float lo = s->y0 + (atGround ? 0.3f : maxf(mv, 0.15f)), hi = s->y1 - (carried ? maxf(mv, 0.15f) : 0.6f);
    float umin = 0.35f, umax = faceW - 0.35f;
    float best = 2.5f * 2.5f;
    int found = 0;
#define TRY(cu, cy) do { \
        float cu_ = (cu), cy_ = (cy), d_ = (cu_ - u) * (cu_ - u) + (cy_ - y) * (cy_ - y); \
        if (cu_ >= umin && cu_ <= umax && cy_ >= lo && cy_ <= hi && d_ < best && !near_door(bg, si, li, k, cu_, cy_)) { \
            best = d_; *uo = cu_; *yo = cy_; found = 1; } \
    } while (0)

    if (atGround) {
        float cy = clampf(y, gy + 0.6f, base - 0.3f);
        int j = (int)floorf(u / r.step);
        if (r.edge >= mu - 0.07f) {   /* the pier, less half a bracket, covers the clearance */
            TRY((float)j * r.step, cy);
            TRY((float)(j + 1) * r.step, cy);
        }
        if (bg->groundMode != GF_RETAIL && 0.5f * (base - r.glassTop) >= mv)
            TRY(u, 0.5f * (r.glassTop + base));   /* lobby glass stops short of the floor above */
    }
    if (!r.cladding) {
        float ylo = maxf(atGround ? base + 0.05f : lo, lo), uc = clampf(u, umin, umax);
        int kf = (int)floorf((y - base) / fh);
        for (int j = kf - 1; j <= kf + 1; ++j) {
            /* solid from the head of floor j's windows to the sill of floor j + 1's, less the clearance */
            float a = maxf(base + fh * ((float)j + sill + winH) + mv, ylo);
            float c = minf(base + fh * ((float)j + 1.0f + sill) - mv, hi);
            if (a <= c) TRY(uc, clampf(y, a, c));
        }
        if (0.5f * (1.0f - winW) * bw >= mu - 0.05f) {   /* wide enough for the bracket and clearance */
            float cy = maxf(y, ylo), rel = cy - base;
            float inFloor = rel - fh * floorf(rel / fh);
            int signs = rel >= 0.0f && rel < 5.0f * fh && inFloor > 0.9f && inFloor < 2.3f;
            if (!signs) {
                int j = (int)floorf(u / bw);
                TRY((float)j * bw, cy);
                TRY((float)(j + 1) * bw, cy);
            }
        }
        int every = 6 + 3 * (s->plant & 15);
        if ((s->plant & 15) && kf > 0 && kf % every == every - 1)
            TRY(u, clampf(y, base + (float)kf * fh + maxf(mv, 0.3f), base + (float)(kf + 1) * fh - maxf(mv, 0.3f)));
    }
#undef TRY
    return found;
}

/* A cable end on solid wall: cast from p along dir at height y, slide along
 * the face that was hit to its nearest solid spot, and check nothing stands
 * in front of that. The end (just off the wall) and its height, or 0; hit,
 * if given, gets the face it is on (u: where along it). */
static int wall_anchor(ChunkGen *g, DVec2 p, DVec2 dir, float y, double maxT, float mu, float mv, DVec2 *at, float *ya, WallHit *hit) {
    WallHit h = facade_ray(g, p, dir, y, maxT);
    if (h.t < 0.0) return 0;
    const Building *b = &g->bld[h.b];
    const BGraph *bg = gen_graph(g, h.b);
    const BSection *s = &bg->sec[h.s];
    EdgeInfo e = edge_of(h.li ? &s->foot.hole : &s->foot.outer, h.k);
    float u2, y2;
    if (!solid_spot(bg, h.s, h.li, h.k, e.len, h.u, y, mu, mv, &u2, &y2)) return 0;
    DVec2 w = to_world(b, e.ax + e.ex * u2, e.az + e.ez * u2);
    DVec2 d = dsub(w, p);
    double dist = dlen(d);
    if (dist < 0.5) return 0;
    WallHit h2 = facade_ray(g, p, dscale(d, 1.0 / dist), y2, dist + 0.5);
    if (h2.t < 0.0 || h2.b != h.b || fabs(h2.t - dist) > 0.05) return 0;
    if (h2.s != h.s) {
        /* stacked sections share a face: the one there at this height must be solid at the same spot */
        const BSection *s2 = &bg->sec[h2.s];
        EdgeInfo e2 = edge_of(h2.li ? &s2->foot.hole : &s2->foot.outer, h2.k);
        float u3, y3;
        if (!solid_spot(bg, h2.s, h2.li, h2.k, e2.len, h2.u, y2, mu, mv, &u3, &y3) || fabsf(u3 - h2.u) > 0.05f || fabsf(y3 - y2) > 0.05f) return 0;
    }
    float nx = -e.ez, nz = e.ex;   /* outward, local */
    *at = dadd(w, dv(0.1 * (b->c * nx - b->s * nz), 0.1 * (b->s * nx + b->c * nz)));
    *ya = y2;
    if (hit) *hit = h2;
    return 1;
}

/* Clear of every lamp: never below the lamp tops within reach of a pole, arm included. */
static int clears_lamps(const ChunkGen *g, DVec2 a, float ya, DVec2 b, float yb, float sag) {
    DVec2 ab = dsub(b, a);
    double l2 = ddot(ab, ab);
    for (int i = 0; i < g->nl; ++i) {
        DVec2 q = dv(g->lamp[i].x, g->lamp[i].z);
        if (seg_dist(q, a, b) > 2.4) continue;
        float t = l2 > 1e-9 ? (float)clampf((float)(ddot(dsub(q, a), ab) / l2), 0.0f, 1.0f) : 0.0f;
        if (lerpf(ya, yb, t) - 4.0f * sag * t * (1.0f - t) < LAMP_HEIGHT + 0.4f) return 0;
    }
    return 1;
}

/* One length of cable, as emit_beam, but each end's vertices carry that end's
 * drop below the chord and the cable's sag in uv, for cableSway. */
static void emit_strand(Mesh *m, vec3 a, float da, vec3 b, float db, float sag, float size, Mat mt) {
    vec3 ax = v3_norm(v3_sub(b, a));
    vec3 h = v3_cross(ax, v3(0, 1, 0));
    h = v3_len(h) < 1e-3f ? v3(1, 0, 0) : v3_norm(h);
    vec3 v = v3_norm(v3_cross(h, ax));
    float r = size * 0.5f;
    vec3 o[4] = { v3_add(v3_scale(h, r), v3_scale(v, r)), v3_add(v3_scale(h, -r), v3_scale(v, r)),
                  v3_add(v3_scale(h, -r), v3_scale(v, -r)), v3_add(v3_scale(h, r), v3_scale(v, -r)) };
    float uv[4][2] = { { da, sag }, { da, sag }, { db, sag }, { db, sag } };
    for (int i = 0; i < 4; ++i) {
        int j = (i + 1) & 3;
        emit_quad(m, v3_add(a, o[i]), v3_add(a, o[j]), v3_add(b, o[j]), v3_add(b, o[i]), v3_norm(v3_add(o[i], o[j])), uv, mt);
    }
}

/* A parabolic sag (close enough to a catenary this taut), as thin beams. The
 * vertex shader swings it about its chord in the gusts (cableSway in
 * shaders/city.vert): params.w flags it, aux holds the direction it swings
 * in (across the chord, horizontal) as an angle in 16 bits. */
static void emit_cable(Mesh *m, const Cable *c, Mat mt) {
    vec3 d = v3_sub(c->b, c->a);
    float span = sqrtf(d.x * d.x + d.z * d.z);
    int n = (int)clampf(span / 3.5f, 5.0f, 12.0f);
    float ang = atan2f(d.x, -d.z);   /* of the horizontal perpendicular (-dz, dx) */
    mt.p[3] = 1;
    mt.aux = (uint32_t)((ang / (2.0f * PI_F) + 0.5f) * 65535.0f + 0.5f);
    vec3 prev = c->a;
    float prevDrop = 0.0f;
    for (int i = 1; i <= n; ++i) {
        float t = (float)i / (float)n, drop = 4.0f * c->sag * t * (1.0f - t);
        vec3 p = v3_add(c->a, v3_scale(d, t));
        p.y -= drop;
        emit_strand(m, prev, prevDrop, p, drop, c->sag, c->thick, mt);
        prev = p;
        prevDrop = drop;
    }
}

/* Wall anchor: a stubby bracket where a cable meets a facade. */
static void emit_anchor(Mesh *m, vec3 p, Mat mt) {
    emit_box(m, v3(p.x - 0.07f, p.y - 0.09f, p.z - 0.07f), v3(p.x + 0.07f, p.y + 0.06f, p.z + 0.07f), F_NOBOT | F_BOT, mt, UV_FACADE);
}

static vec3 chunk_pt(const ChunkGen *g, DVec2 p, float y) { return v3((float)(p.x - g->ox), y, (float)(p.z - g->oz)); }

/* How far a span sags: its share of the span, but never below `clear`. */
static float span_sag(DVec2 a, float ya, DVec2 b, float yb, float sagFrac, float clear) {
    return maxf(minf((float)dlen(dsub(b, a)) * sagFrac, minf(ya, yb) - clear), 0.1f);
}

/* Wall to wall, with brackets at both ends. */
static void emit_span(Mesh *m, const ChunkGen *g, DVec2 a, float ya, DVec2 b, float yb, float sag, float thick, Mat mt) {
    Cable c;
    c.a = chunk_pt(g, a, ya);
    c.b = chunk_pt(g, b, yb);
    c.sag = sag;
    c.thick = thick;
    emit_cable(m, &c, mt);
    emit_anchor(m, c.a, mt);
    emit_anchor(m, c.b, mt);
}

/* One strand across road `rd` from station q (on the centre line, chunk-owned). */
static void cross_strand(Mesh *m, ChunkGen *g, const Road *rd, DVec2 q, DVec2 dir, DVec2 nrm,
                         double skew, float ya, float yb, float sagFrac, float thick, Mat mt) {
    double maxT = rd->at.half + rd->at.walk + 14.0, minT = rd->at.half + 0.5;
    DVec2 qb = dadd(q, dscale(dir, skew)), back = dscale(nrm, -1.0), a, b;
    if (!wall_anchor(g, q, nrm, ya, maxT, STREET_GAP, STREET_GAP, &a, &ya, NULL) || !wall_anchor(g, qb, back, yb, maxT, STREET_GAP, STREET_GAP, &b, &yb, NULL)) return;
    if (ddot(dsub(a, q), nrm) < minT || ddot(dsub(b, qb), back) < minT || minf(ya, yb) < CABLE_CLEAR + 0.3f) return;
    float sag = span_sag(a, ya, b, yb, sagFrac, CABLE_CLEAR);
    if (!clears_lamps(g, a, ya, b, yb, sag)) return;
    emit_span(m, g, a, ya, b, yb, sag, thick, mt);
}

static void build_street_cables(Mesh *m, ChunkGen *g) {
    const double P = CHUNK_SIZE;
    Lamp lt[128];
    for (int ri = 0; ri < g->pa->nroads; ++ri) {
        const Road *rd = &g->pa->roads[ri];
        DVec2 d = dsub(rd->b, rd->a);
        double len = dlen(d);
        if (len < 1.0) continue;
        DVec2 dir = dscale(d, 1.0 / len), nrm = dv(-dir.z, dir.x);
        uint32_t seedR = hash3i(0xCAB1E5u ^ rd->at.id, (int32_t)floor(rd->a.x), (int32_t)floor(rd->a.z), (int32_t)floor(rd->b.x));

        /* Crossings, facade to facade. */
        Rng step = rng_make(seedR);
        double s1 = len - rd->trimB - 3.0;
        int k = 0;
        for (double s = rd->trimA + 3.0 + rng_range(&step, 0.0, 8.0); s < s1; s += rng_range(&step, 5.0f, 13.0f), ++k) {
            DVec2 q = dadd(rd->a, dscale(dir, s));
            if (!owns(g, q.x, q.z)) continue;
            Rng r = rng_make(hash_combine(seedR, (uint32_t)k));
            float dens = plan_density(q.x, q.z);
            if (rng_float(&r) >= minf(0.1f + 0.7f * dens, 0.7f)) continue;
            float roll = rng_float(&r);
            int strands = roll < 0.55f ? 1 : roll < 0.82f ? 2 : 3;
            int high = rng_float(&r) < 0.3f * dens;
            float y = high ? rng_range(&r, 18.0f, 42.0f) : rng_range(&r, 5.8f, 15.5f);
            float dy = rng_range(&r, -2.0f, 2.0f);
            double skew = rng_range(&r, -5.0f, 5.0f);
            float sagFrac = rng_range(&r, 0.025f, 0.065f);
            float thick = rng_range(&r, 0.045f, 0.075f);
            float bunS = rng_range(&r, 0.25f, 0.6f), bunY = rng_range(&r, -0.45f, 0.45f);
            Mat mt = mat(MAT_METAL, seedR + (uint32_t)k, rng_float(&r) < 0.75f ? 3 : 2, 0, 0);
            for (int j = 0; j < strands; ++j) {
                DVec2 qj = dadd(q, dscale(dir, bunS * j));
                float ya = y + bunY * j, yb = maxf(ya + dy, CABLE_CLEAR + 0.8f);
                cross_strand(m, g, rd, qj, dir, nrm, skew, ya, yb, sagFrac * (1.0f + 0.35f * j), thick * (j ? 0.8f : 1.0f), mt);
            }
        }

        /* Lamp to lamp along each sidewalk, and from lamps up to the wall behind. */
        int nl = city_road_lamps(g->pa, ri, g->ox - 60.0, g->oz - 60.0, g->ox + P + 60.0, g->oz + P + 60.0, lt, 128);
        for (int i = 0; i < nl; ++i) {
            const Lamp *A = &lt[i];
            if (!owns(g, A->x, A->z)) continue;
            Rng r = rng_make(hash2i(seedR ^ 0x1A4Fu, (int32_t)floor(A->x * 4.0), (int32_t)floor(A->z * 4.0)));
            float dens = plan_density(A->x, A->z);
            const float top = LAMP_HEIGHT - 0.25f;
            Mat mt = mat(MAT_METAL, seedR ^ (uint32_t)i, 3, 0, 0);
            if (rng_float(&r) < 0.3f + 0.35f * dens) {
                for (int j = i + 1; j < nl && j <= i + 3; ++j) {
                    const Lamp *B = &lt[j];
                    double dd = hypot(B->x - A->x, B->z - A->z);
                    if (A->dx * B->dx + A->dz * B->dz < 0.9f || dd > LAMP_SPACING * 1.3) continue;
                    Cable c;
                    c.a = chunk_pt(g, dv(A->x, A->z), top);
                    c.b = chunk_pt(g, dv(B->x, B->z), top);
                    c.sag = (float)dd * rng_range(&r, 0.035f, 0.06f);
                    c.thick = rng_range(&r, 0.035f, 0.055f);
                    emit_cable(m, &c, mt);
                    break;
                }
            }
            if (rng_float(&r) < 0.12f + 0.25f * dens) {
                /* feeder: up from the lamp to the facade behind it */
                DVec2 back = dv(-A->dx, -A->dz), w;
                float y;
                if (wall_anchor(g, dv(A->x, A->z), back, rng_range(&r, 8.2f, 12.0f), rd->at.walk + 14.0, STREET_GAP, STREET_GAP, &w, &y, NULL) && y > top + 0.5f) {
                    Cable c;
                    c.a = chunk_pt(g, dv(A->x, A->z), top);
                    c.b = chunk_pt(g, w, y);
                    c.sag = (float)dlen(dsub(w, dv(A->x, A->z))) * 0.08f;
                    c.thick = 0.045f;
                    emit_cable(m, &c, mt);
                    emit_anchor(m, c.b, mt);
                }
            }
        }
    }
}

/* Inside a block's buildable lot: an alley or courtyard, not a street or sidewalk. */
static int in_lot(const ChunkGen *g, DVec2 p) {
    for (int k = 0; k < g->pa->nblocks; ++k) {
        const Block *bk = &g->pa->blocks[k];
        if (!bk->lot.n || p.x < bk->x0 || p.x > bk->x1 || p.z < bk->z0 || p.z > bk->z1) continue;
        if (poly_contains(&bk->lot, p.x, p.z, 0.0)) return 1;
    }
    return 0;
}

/* A face in the world: its start corner, the unit vectors along and out of it. */
typedef struct WallFrame { DVec2 o, t, n; float len; const BGraph *bg; int s; } WallFrame;

static WallFrame wall_frame(ChunkGen *g, int bi, int si, int li, int k) {
    const Building *b = &g->bld[bi];
    WallFrame w;
    w.bg = gen_graph(g, bi);
    w.s = si;
    const BSection *s = &w.bg->sec[si];
    EdgeInfo e = edge_of(li ? &s->foot.hole : &s->foot.outer, k);
    w.o = to_world(b, e.ax, e.az);
    w.t = dv(b->c * e.ex - b->s * e.ez, b->s * e.ex + b->c * e.ez);
    w.n = dv(-w.t.z, w.t.x);   /* outward is (-dz, dx) */
    w.len = e.len;
    return w;
}

/* In a narrow alley a cable runs close along the walls near its ends: wherever
 * it passes within 0.35 m of wall w, sag included, the facade behind it must be
 * solid, not just where it is fixed. */
static int clear_of_glass(const WallFrame *w, DVec2 a, float ya, DVec2 b, float yb, float sag) {
    for (int i = 0; i <= 32; ++i) {
        float t = (float)i / 32.0f;
        DVec2 p = dadd(a, dscale(dsub(b, a), t));
        double d = ddot(dsub(p, w->o), w->n), u = ddot(dsub(p, w->o), w->t);
        if (d > 0.35 || u < 0.0 || u > w->len) continue;
        if (glass_near(w->bg, w->s, w->len, (float)u, lerpf(ya, yb, t) - 4.0f * sag * t * (1.0f - t), 0.06f, 0.06f)) return 0;
    }
    return 1;
}

/* Alleys and courtyards: from stations along each low wall of the chunk's
 * buildings to whatever wall faces it across the gap, skewed so the lines
 * criss-cross, and hung lower and slacker than over the streets. Stricter than
 * over the streets, where the walls are far apart: both ends must have solid
 * wall all round them, and the cable must stay clear of glass along the walls. */
static void build_alley_cables(Mesh *m, ChunkGen *g) {
    for (int i = 0; i < g->nb; ++i) {
        const Building *b = &g->bld[i];
        if (!owns(g, b->ox, b->oz)) continue;
        const BGraph *bg = gen_graph(g, i);
        float pCable = 0.3f + 0.45f * b->density;
        for (int si = 0; si < bg->nsec; ++si) {
            const BSection *s = &bg->sec[si];
            if (s->y0 > 14.0f || s->y1 < 5.0f) continue;
            const BLoop *loops[2] = { &s->foot.outer, &s->foot.hole };
            for (int li = 0; li < 2; ++li) {
                const BLoop *l = loops[li];
                for (int k = 0; k < l->n; ++k) {
                    EdgeInfo e = edge_of(l, k);
                    if (e.len < 3.0f) continue;
                    uint32_t seedE = hash3i(bg->seed ^ 0xA11E7u, si, li, k);
                    Rng step = rng_make(seedE);
                    int idx = 0;
                    for (float u = rng_range(&step, 0.8f, 3.0f); u < e.len - 0.8f; u += rng_range(&step, 2.5f, 6.0f), ++idx) {
                        Rng r = rng_make(hash_combine(seedE, (uint32_t)idx));
                        if (rng_float(&r) >= pCable) continue;
                        /* mostly low: the first few floors are where alleys tangle */
                        float ylo = maxf(3.8f, s->y0), yhi = minf(s->y1, 24.0f);
                        float ya0 = ylo + maxf(yhi - ylo, 0.0f) * powf(rng_float(&r), 1.8f), dy = rng_range(&r, -3.0f, 3.0f);
                        float skew = rng_range(&r, -0.7f, 0.7f), sagFrac = rng_range(&r, 0.04f, 0.1f);
                        float thick = rng_range(&r, 0.035f, 0.065f);
                        int strands = rng_float(&r) < 0.3f ? 2 : 1;
                        Mat mt = mat(MAT_METAL, seedE + (uint32_t)idx, rng_float(&r) < 0.7f ? 3 : 2, 0, 0);
                        for (int j = 0; j < strands; ++j) {
                            float uj, ya, yb;
                            if (!solid_spot(bg, si, li, k, e.len, u + 0.3f * (float)j, ya0, ALLEY_GAP_U, ALLEY_GAP_V, &uj, &ya) ||
                                glass_near(bg, si, e.len, uj, ya, ALLEY_GAP_U - 0.05f, ALLEY_GAP_V - 0.05f)) continue;
                            /* the wall there must be this section's, not behind another one */
                            float lx = e.ax + e.ex * uj - e.ez * 0.02f, lz = e.az + e.ez * uj + e.ex * 0.02f;   /* outward is (-dz, dx) */
                            int covered = 0;
                            for (int sj = 0; sj < bg->nsec && !covered; ++sj)
                                covered = sj != si && ya >= bg->sec[sj].y0 && ya <= bg->sec[sj].y1 && bfoot_contains(&bg->sec[sj].foot, lx, lz);
                            if (covered) continue;
                            DVec2 pw = to_world(b, lx, lz);
                            float nx = -e.ez + e.ex * skew, nz = e.ex + e.ez * skew, nn = sqrtf(nx * nx + nz * nz);
                            DVec2 dir = dv((b->c * nx - b->s * nz) / nn, (b->s * nx + b->c * nz) / nn), pb;
                            WallHit hb;
                            if (!wall_anchor(g, pw, dir, maxf(ya + dy, 4.0f), 14.0, ALLEY_GAP_U, ALLEY_GAP_V, &pb, &yb, &hb) || dlen(dsub(pb, pw)) < 1.8) continue;
                            if (minf(ya, yb) < ALLEY_CLEAR + 0.4f || !in_lot(g, dscale(dadd(pw, pb), 0.5))) continue;
                            WallFrame wa = wall_frame(g, i, si, li, k), wb = wall_frame(g, hb.b, hb.s, hb.li, hb.k);
                            if (glass_near(wb.bg, wb.s, wb.len, hb.u, yb, ALLEY_GAP_U - 0.05f, ALLEY_GAP_V - 0.05f)) continue;
                            DVec2 pa = to_world(b, lx - e.ez * 0.08f, lz + e.ex * 0.08f);
                            float sag = span_sag(pa, ya, pb, yb, sagFrac * (1.0f + 0.4f * (float)j), ALLEY_CLEAR);
                            if (!clear_of_glass(&wa, pa, ya, pb, yb, sag) || !clear_of_glass(&wb, pa, ya, pb, yb, sag)) continue;
                            emit_span(m, g, pa, ya, pb, yb, sag, thick, mt);
                        }
                    }
                }
            }
        }
    }
}

static void build_cables(Mesh *m, ChunkGen *g) {
    build_street_cables(m, g);
    build_alley_cables(m, g);
}

/* ===========================================================================
 * Chunk mesh
 * ========================================================================= */

void city_build_mesh(int cx, int cz, Mesh *m) {
    ChunkGen g;
    gen_init(&g, cx, cz, 130.0, 90.0, 105.0);
    m->count = 0;
    m->xf = 0;
    m->mn[0] = m->mn[1] = m->mn[2] = 1e30f;
    m->mx[0] = m->mx[1] = m->mx[2] = -1e30f;
    free(m->lights);
    m->lights = NULL;
    m->nlights = 0;

    build_roads(m, &g);
    build_blocks(m, &g);
    for (int i = 0; i < g.nl; ++i)
        if (owns(&g, g.lamp[i].x, g.lamp[i].z)) build_lamp(m, &g, &g.lamp[i], (uint32_t)i);
    for (int i = 0; i < g.nb; ++i) {
        const Building *b = &g.bld[i];
        if (!owns(&g, b->ox, b->oz)) continue;
        m->xf = 1;
        m->xc = b->c; m->xs = b->s;
        m->xx = (float)(b->ox - g.ox); m->xz = (float)(b->oz - g.oz);
        emit_building(m, b, gen_graph(&g, i));
        m->xf = 0;
    }
    build_cables(m, &g);
    build_lights(m, &g);
    gen_free(&g);
}

/* ===========================================================================
 * Spawn point
 * ========================================================================= */

void city_spawn(double *x, double *z, float *yaw) {
    DVec2 c = plan_town_centre(0, 0);
    *x = c.x; *z = c.z; *yaw = 0.0f;
    PlanArea *pa = (PlanArea *)malloc(sizeof(PlanArea));
    if (!pa) fatal("out of memory finding a spawn point");
    plan_area(c.x - 320.0, c.z - 320.0, c.x + 320.0, c.z + 320.0, pa);
    /* The nearest decent street to the town centre, on its sidewalk. */
    double best = 1e300;
    for (int ri = 0; ri < pa->nroads; ++ri) {
        const Road *r = &pa->roads[ri];
        DVec2 d = dsub(r->b, r->a);
        double len = dlen(d);
        if (len - r->trimA - r->trimB < 50.0) continue;
        DVec2 dir = dscale(d, 1.0 / len), nrm = dv(-dir.z, dir.x);
        double s = (r->trimA + len - r->trimB) * 0.5 + LAMP_SPACING * 0.5;
        for (int side = -1; side <= 1; side += 2) {
            DVec2 q = dadd(dadd(r->a, dscale(dir, s)), dscale(nrm, side * (r->at.half + r->at.walk * 0.55)));
            double dist = dlen(dsub(q, c));
            if (dist >= best || q.x < pa->x0 + 40 || q.x > pa->x1 - 40 || q.z < pa->z0 + 40 || q.z > pa->z1 - 40) continue;
            int onWalk = 0;
            for (int k = 0; k < pa->nblocks && !onWalk; ++k) {
                const Block *bk = &pa->blocks[k];
                onWalk = poly_contains(&bk->curb, q.x, q.z, -0.5) && !(bk->lot.n && poly_contains(&bk->lot, q.x, q.z, 0.6));
            }
            if (!onWalk) continue;
            best = dist;
            *x = q.x; *z = q.z;
            *yaw = (float)atan2(dir.x, -dir.z);
        }
    }
    free(pa);
}
