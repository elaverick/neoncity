#include "building_internal.h"

/* ===========================================================================
 * Polygons (simple, possibly non-convex)
 * ========================================================================= */

float bloop_area(const BLoop *l) {
    double s = 0.0;
    for (int i = 0; i < l->n; ++i) {
        int j = (i + 1) % l->n;
        s += (double)l->x[i] * l->z[j] - (double)l->z[i] * l->x[j];
    }
    return (float)(0.5 * s);
}

void loop_add(BLoop *l, float x, float z) {
    if (l->n < BG_MAX_PV) { l->x[l->n] = x; l->z[l->n] = z; l->n++; }
}

static void loop_reverse(BLoop *l) {
    for (int i = 0, j = l->n - 1; i < j; ++i, --j) {
        float tx = l->x[i], tz = l->z[i];
        l->x[i] = l->x[j]; l->z[i] = l->z[j];
        l->x[j] = tx; l->z[j] = tz;
    }
}

/* Outer loops negative area, courtyard loops positive (see building.h). */
void loop_orient(BLoop *l, int outer) {
    float a = bloop_area(l);
    if ((outer && a > 0.0f) || (!outer && a < 0.0f)) loop_reverse(l);
}

int bloop_inside(const BLoop *l, float x, float z) {
    int in = 0;
    for (int i = 0, j = l->n - 1; i < l->n; j = i++) {
        if ((l->z[i] > z) != (l->z[j] > z)) {
            float xc = l->x[j] + (z - l->z[j]) * (l->x[i] - l->x[j]) / (l->z[i] - l->z[j]);
            if (x < xc) in = !in;
        }
    }
    return in;
}

int bfoot_contains(const BFoot *f, float x, float z) {
    return bloop_inside(&f->outer, x, z) && !(f->hole.n && bloop_inside(&f->hole, x, z));
}

static float seg_d2(float px, float pz, float ax, float az, float bx, float bz, float *qx, float *qz) {
    float ex = bx - ax, ez = bz - az, l2 = ex * ex + ez * ez;
    float t = l2 > 1e-12f ? ((px - ax) * ex + (pz - az) * ez) / l2 : 0.0f;
    t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
    float x = ax + ex * t, z = az + ez * t;
    if (qx) { *qx = x; *qz = z; }
    return (px - x) * (px - x) + (pz - z) * (pz - z);
}

float loop_dist2(const BLoop *l, float x, float z, float *qx, float *qz, float best) {
    for (int i = 0; i < l->n; ++i) {
        int j = (i + 1) % l->n;
        float cx, cz, d = seg_d2(x, z, l->x[i], l->z[i], l->x[j], l->z[j], &cx, &cz);
        if (d < best) { best = d; if (qx) { *qx = cx; *qz = cz; } }
    }
    return best;
}

float bfoot_boundary_dist(const BFoot *f, float x, float z, float *qx, float *qz) {
    float d = loop_dist2(&f->outer, x, z, qx, qz, 1e30f);
    if (f->hole.n) d = loop_dist2(&f->hole, x, z, qx, qz, d);
    return sqrtf(d);
}

static float orient2(float ax, float az, float bx, float bz, float cx, float cz) {
    return (bx - ax) * (cz - az) - (bz - az) * (cx - ax);
}

static int segs_cross(float ax, float az, float bx, float bz, float cx, float cz, float dx, float dz) {
    float o1 = orient2(ax, az, bx, bz, cx, cz), o2 = orient2(ax, az, bx, bz, dx, dz);
    float o3 = orient2(cx, cz, dx, dz, ax, az), o4 = orient2(cx, cz, dx, dz, bx, bz);
    const float e = 1e-5f;
    return ((o1 > e && o2 < -e) || (o1 < -e && o2 > e)) && ((o3 > e && o4 < -e) || (o3 < -e && o4 > e));
}

int loop_simple(const BLoop *l) {
    for (int i = 0; i < l->n; ++i)
        for (int j = i + 2; j < l->n; ++j) {
            if (i == 0 && j == l->n - 1) continue;
            int i1 = (i + 1) % l->n, j1 = (j + 1) % l->n;
            if (segs_cross(l->x[i], l->z[i], l->x[i1], l->z[i1], l->x[j], l->z[j], l->x[j1], l->z[j1])) return 0;
        }
    return 1;
}

float loop_min_edge(const BLoop *l) {
    float m = 1e30f;
    for (int i = 0; i < l->n; ++i) {
        int j = (i + 1) % l->n;
        float d = sqrtf((l->x[j] - l->x[i]) * (l->x[j] - l->x[i]) + (l->z[j] - l->z[i]) * (l->z[j] - l->z[i]));
        if (d < m) m = d;
    }
    return m;
}

float loop_perimeter(const BLoop *l) {
    float p = 0.0f;
    for (int i = 0; i < l->n; ++i) {
        int j = (i + 1) % l->n;
        p += sqrtf((l->x[j] - l->x[i]) * (l->x[j] - l->x[i]) + (l->z[j] - l->z[i]) * (l->z[j] - l->z[i]));
    }
    return p;
}

void loop_bounds(const BLoop *l, float *x0, float *z0, float *x1, float *z1) {
    *x0 = *z0 = 1e30f; *x1 = *z1 = -1e30f;
    for (int i = 0; i < l->n; ++i) {
        *x0 = minf(*x0, l->x[i]); *x1 = maxf(*x1, l->x[i]);
        *z0 = minf(*z0, l->z[i]); *z1 = maxf(*z1, l->z[i]);
    }
}

int loops_intersect(const BLoop *a, const BLoop *b) {
    for (int i = 0; i < a->n; ++i) {
        int i1 = (i + 1) % a->n;
        for (int j = 0; j < b->n; ++j) {
            int j1 = (j + 1) % b->n;
            if (segs_cross(a->x[i], a->z[i], a->x[i1], a->z[i1], b->x[j], b->z[j], b->x[j1], b->z[j1])) return 1;
        }
    }
    for (int i = 0; i < a->n; ++i) if (bloop_inside(b, a->x[i], a->z[i])) return 1;
    for (int j = 0; j < b->n; ++j) if (bloop_inside(a, b->x[j], b->z[j])) return 1;
    return 0;
}

/* The narrowest calliper width: the least extent across any direction, which
 * is the least over the convex hull's edges of the farthest vertex from it. */
float bloop_width(const BLoop *l) {
    int h[BG_MAX_PV + 1], n = 0, ord[BG_MAX_PV];
    for (int i = 0; i < l->n; ++i) ord[i] = i;
    for (int i = 1; i < l->n; ++i)            /* by x, then z */
        for (int j = i; j > 0 && (l->x[ord[j]] < l->x[ord[j - 1]] || (l->x[ord[j]] == l->x[ord[j - 1]] && l->z[ord[j]] < l->z[ord[j - 1]])); --j) {
            int t = ord[j]; ord[j] = ord[j - 1]; ord[j - 1] = t;
        }
    for (int pass = 0; pass < 2; ++pass) {     /* monotone chain: lower hull, then upper */
        int start = n;
        for (int k = 0; k < l->n; ++k) {
            int i = pass ? ord[l->n - 1 - k] : ord[k];
            while (n - start >= 2 && orient2(l->x[h[n - 2]], l->z[h[n - 2]], l->x[h[n - 1]], l->z[h[n - 1]], l->x[i], l->z[i]) <= 0.0f) n--;
            h[n++] = i;
        }
        n--;                                    /* the last point starts the other chain */
    }
    if (n < 3) return 0.0f;
    float best = 1e30f;
    for (int e = 0; e < n; ++e) {
        int a = h[e], b = h[(e + 1) % n];
        float ex = l->x[b] - l->x[a], ez = l->z[b] - l->z[a], len = sqrtf(ex * ex + ez * ez);
        if (len < 1e-5f) continue;
        float far = 0.0f;
        for (int k = 0; k < n; ++k)
            far = maxf(far, fabsf(orient2(l->x[a], l->z[a], l->x[b], l->z[b], l->x[h[k]], l->z[h[k]])) / len);
        best = minf(best, far);
    }
    return best;
}

int loop_convex(const BLoop *l) {
    for (int i = 0; i < l->n; ++i) {
        int j = (i + 1) % l->n, k = (i + 2) % l->n;
        if (orient2(l->x[i], l->z[i], l->x[j], l->z[j], l->x[k], l->z[k]) > 1e-4f) return 0;  /* outer loops turn negative */
    }
    return 1;
}

/* Ear clipping; a courtyard is joined to the outer loop by a bridge first. */
int bfoot_triangulate(const BFoot *f, float (*pts)[2], int *npts, int (*tri)[3], int maxTri) {
    int no = f->outer.n, nh = f->hole.n, np = 0;
    for (int i = 0; i < no; ++i) { pts[np][0] = f->outer.x[i]; pts[np][1] = f->outer.z[i]; np++; }
    for (int i = 0; i < nh; ++i) { pts[np][0] = f->hole.x[i]; pts[np][1] = f->hole.z[i]; np++; }
    *npts = np;
    int idx[2 * BG_MAX_PV + 4], n = 0;
    if (!nh) {
        for (int i = 0; i < no; ++i) idx[n++] = i;
    } else {
        /* Bridge from the hole vertex with the largest x to the nearest outer
         * vertex that can see it without crossing any edge. */
        int hm = 0;
        for (int i = 1; i < nh; ++i) if (f->hole.x[i] > f->hole.x[hm]) hm = i;
        float hx = f->hole.x[hm], hz = f->hole.z[hm], best = 1e30f;
        int ov = 0;
        for (int i = 0; i < no; ++i) {
            float d = (f->outer.x[i] - hx) * (f->outer.x[i] - hx) + (f->outer.z[i] - hz) * (f->outer.z[i] - hz);
            if (d >= best) continue;
            int blocked = 0;
            for (int k = 0; k < no && !blocked; ++k) {
                int k1 = (k + 1) % no;
                if (k == i || k1 == i) continue;
                blocked = segs_cross(hx, hz, f->outer.x[i], f->outer.z[i], f->outer.x[k], f->outer.z[k], f->outer.x[k1], f->outer.z[k1]);
            }
            for (int k = 0; k < nh && !blocked; ++k) {
                int k1 = (k + 1) % nh;
                if (k == hm || k1 == hm) continue;
                blocked = segs_cross(hx, hz, f->outer.x[i], f->outer.z[i], f->hole.x[k], f->hole.z[k], f->hole.x[k1], f->hole.z[k1]);
            }
            if (!blocked) { best = d; ov = i; }
        }
        for (int i = 0; i <= ov; ++i) idx[n++] = i;
        for (int k = 0; k <= nh; ++k) idx[n++] = no + (hm + k) % nh;
        for (int i = ov; i < no; ++i) idx[n++] = i;
    }
    float area = 0.0f;
    for (int i = 0; i < n; ++i) {
        int j = (i + 1) % n;
        area += pts[idx[i]][0] * pts[idx[j]][1] - pts[idx[i]][1] * pts[idx[j]][0];
    }
    float sgn = area < 0.0f ? -1.0f : 1.0f;
    int nt = 0, guard = 0;
    while (n > 3 && nt < maxTri && guard++ < 4 * 2 * BG_MAX_PV * 2 * BG_MAX_PV) {
        int found = -1;
        for (int i = 0; i < n && found < 0; ++i) {
            int a = idx[(i + n - 1) % n], b = idx[i], c = idx[(i + 1) % n];
            float ax = pts[a][0], az = pts[a][1], bx = pts[b][0], bz = pts[b][1], cx = pts[c][0], cz = pts[c][1];
            if (orient2(ax, az, bx, bz, cx, cz) * sgn <= 1e-6f) continue;          /* reflex */
            int clear = 1;
            for (int k = 0; k < n && clear; ++k) {
                int p = idx[k];
                float px = pts[p][0], pz = pts[p][1];
                if ((px == ax && pz == az) || (px == bx && pz == bz) || (px == cx && pz == cz)) continue;
                float o1 = orient2(ax, az, bx, bz, px, pz) * sgn, o2 = orient2(bx, bz, cx, cz, px, pz) * sgn,
                      o3 = orient2(cx, cz, ax, az, px, pz) * sgn;
                if (o1 > 0.0f && o2 > 0.0f && o3 > 0.0f) clear = 0;
            }
            if (clear) found = i;
        }
        if (found < 0) found = 0;   /* degenerate remainder: clip anyway rather than loop */
        tri[nt][0] = idx[(found + n - 1) % n]; tri[nt][1] = idx[found]; tri[nt][2] = idx[(found + 1) % n];
        nt++;
        for (int k = found; k < n - 1; ++k) idx[k] = idx[k + 1];
        n--;
    }
    if (n == 3 && nt < maxTri) { tri[nt][0] = idx[0]; tri[nt][1] = idx[1]; tri[nt][2] = idx[2]; nt++; }
    return nt;
}
