#include "city_internal.h"

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
        if (poly_contains(&bk->curb, q.x, q.z, -0.3) && !(bk->lot.n && poly_contains(&bk->lot, q.x, q.z, 0.3))) {
            /* not in a drop kerb's way */
            DropKerb dk[MAX_BLOCK_DROPS];
            int nd = city_block_drops(pa, bk, dk, MAX_BLOCK_DROPS);
            for (int i = 0; i < nd; ++i)
                if (city_drop_contains(&dk[i], q.x, q.z, 0.6)) return 0;
            return 1;
        }
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
