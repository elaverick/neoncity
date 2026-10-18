#include "city_internal.h"

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
    b->base = plot_base(b);
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
                H = lerpf(14.0f, 30.0f + 70.0f * d, rng_float(&f->rng));
                H *= 0.7f + 0.6f * rng_float(&f->rng);
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
