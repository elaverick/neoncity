#include "pmodel.h"
#include "hash.h"
#include "mathlib.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ===========================================================================
 * The kit's bake and checks (pmodel.h): a BVH ray caster over a mesh, the
 * per-vertex occlusion and wear terms, modules' instances built into a mesh,
 * and the statistics the lab checks.
 * ========================================================================= */

static float dot3(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static void cross3(float o[3], const float a[3], const float b[3]) {
    float x = a[1] * b[2] - a[2] * b[1], y = a[2] * b[0] - a[0] * b[2], z = a[0] * b[1] - a[1] * b[0];
    o[0] = x; o[1] = y; o[2] = z;
}
static void sub3(float o[3], const float a[3], const float b[3]) { o[0] = a[0] - b[0]; o[1] = a[1] - b[1]; o[2] = a[2] - b[2]; }
static int norm3(float a[3]) { float l = sqrtf(dot3(a, a)); if (l < 1e-12f) return 0; a[0] /= l; a[1] /= l; a[2] /= l; return 1; }

/* ----------------------------------------------------------------- bvh --- */
typedef struct BNode { float lo[3], hi[3]; uint32_t left, first, count; } BNode;   /* count 0: inner (children left, left + 1) */
struct PmBvh { BNode *node; uint32_t nnode; float (*tri)[9]; uint32_t *order; uint32_t ntri; };

static void tri_bounds(const float t[9], float lo[3], float hi[3]) {
    for (int k = 0; k < 3; ++k) {
        lo[k] = minf(t[k], minf(t[3 + k], t[6 + k]));
        hi[k] = maxf(t[k], maxf(t[3 + k], t[6 + k]));
    }
}

static void bvh_split(PmBvh *b, uint32_t ni, float (*cen)[3]) {
    BNode *n = &b->node[ni];
    float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f }, clo[3] = { 1e30f, 1e30f, 1e30f }, chi[3] = { -1e30f, -1e30f, -1e30f };
    for (uint32_t i = n->first; i < n->first + n->count; ++i) {
        float tl[3], th[3];
        tri_bounds(b->tri[b->order[i]], tl, th);
        for (int k = 0; k < 3; ++k) {
            lo[k] = minf(lo[k], tl[k]); hi[k] = maxf(hi[k], th[k]);
            clo[k] = minf(clo[k], cen[b->order[i]][k]); chi[k] = maxf(chi[k], cen[b->order[i]][k]);
        }
    }
    memcpy(n->lo, lo, sizeof(lo)); memcpy(n->hi, hi, sizeof(hi));
    if (n->count <= 4) return;
    /* the split: binned by centroid on each axis, the plane of least surface area cost (a mesh of big thin
     * sheets and small parts is badly served by the middle); the middle of the longest axis if none helps */
    enum { NBIN = 12 };
    int ax = 0;
    for (int k = 1; k < 3; ++k) if (chi[k] - clo[k] > chi[ax] - clo[ax]) ax = k;
    float mid = 0.5f * (clo[ax] + chi[ax]), bestCost = 1e30f;
    for (int a = 0; a < 3; ++a) {
        float ext = chi[a] - clo[a];
        if (ext < 1e-6f) continue;
        uint32_t cnt[NBIN] = { 0 };
        float blo[NBIN][3], bhi[NBIN][3];
        for (int q = 0; q < NBIN; ++q) for (int k = 0; k < 3; ++k) { blo[q][k] = 1e30f; bhi[q][k] = -1e30f; }
        for (uint32_t i = n->first; i < n->first + n->count; ++i) {
            int q = (int)((cen[b->order[i]][a] - clo[a]) / ext * (float)NBIN);
            q = q < 0 ? 0 : q >= NBIN ? NBIN - 1 : q;
            float tl[3], th[3];
            tri_bounds(b->tri[b->order[i]], tl, th);
            cnt[q]++;
            for (int k = 0; k < 3; ++k) { blo[q][k] = minf(blo[q][k], tl[k]); bhi[q][k] = maxf(bhi[q][k], th[k]); }
        }
        for (int s = 1; s < NBIN; ++s) {
            float L0[3] = { 1e30f, 1e30f, 1e30f }, L1[3] = { -1e30f, -1e30f, -1e30f }, R0[3] = { 1e30f, 1e30f, 1e30f }, R1[3] = { -1e30f, -1e30f, -1e30f };
            uint32_t nl = 0, nr = 0;
            for (int q = 0; q < NBIN; ++q) {
                if (!cnt[q]) continue;
                float *o0 = q < s ? L0 : R0, *o1 = q < s ? L1 : R1;
                for (int k = 0; k < 3; ++k) { o0[k] = minf(o0[k], blo[q][k]); o1[k] = maxf(o1[k], bhi[q][k]); }
                if (q < s) nl += cnt[q]; else nr += cnt[q];
            }
            if (!nl || !nr) continue;
            float dl[3] = { L1[0] - L0[0], L1[1] - L0[1], L1[2] - L0[2] }, dr[3] = { R1[0] - R0[0], R1[1] - R0[1], R1[2] - R0[2] };
            float cost = (dl[0] * dl[1] + dl[1] * dl[2] + dl[2] * dl[0]) * (float)nl + (dr[0] * dr[1] + dr[1] * dr[2] + dr[2] * dr[0]) * (float)nr;
            if (cost < bestCost) { bestCost = cost; ax = a; mid = clo[a] + ext * (float)s / (float)NBIN; }
        }
    }
    uint32_t i = n->first, j = n->first + n->count;
    while (i < j) {
        if (cen[b->order[i]][ax] < mid) i++;
        else { uint32_t t = b->order[i]; b->order[i] = b->order[--j]; b->order[j] = t; }
    }
    uint32_t nl = i - n->first;
    if (nl == 0 || nl == n->count) nl = n->count / 2;   /* all centroids alike: split the list */
    uint32_t l = b->nnode;
    b->nnode += 2;
    BNode *L = &b->node[l], *R = &b->node[l + 1];
    L->first = n->first; L->count = nl; R->first = n->first + nl; R->count = n->count - nl;
    n->left = l; n->count = 0;
    bvh_split(b, l, cen);
    bvh_split(b, l + 1, cen);
}

PmBvh *pm_bvh_build(const PmMesh *m) {
    PmBvh *b = (PmBvh *)calloc(1, sizeof(PmBvh));
    b->ntri = m->ni / 3;
    if (!b->ntri) return b;
    b->tri = (float (*)[9])malloc(sizeof(float) * 9 * b->ntri);
    b->order = (uint32_t *)malloc(sizeof(uint32_t) * b->ntri);
    b->node = (BNode *)malloc(sizeof(BNode) * 2 * b->ntri);
    float (*cen)[3] = (float (*)[3])malloc(sizeof(float) * 3 * b->ntri);
    for (uint32_t t = 0; t < b->ntri; ++t) {
        for (int k = 0; k < 3; ++k) memcpy(&b->tri[t][k * 3], m->v[m->idx[t * 3 + k]].p, sizeof(float) * 3);
        for (int k = 0; k < 3; ++k) cen[t][k] = (b->tri[t][k] + b->tri[t][3 + k] + b->tri[t][6 + k]) / 3.0f;
        b->order[t] = t;
    }
    b->nnode = 1;
    b->node[0].first = 0; b->node[0].count = b->ntri; b->node[0].left = 0;
    bvh_split(b, 0, cen);
    free(cen);
    return b;
}

void pm_bvh_free(PmBvh *b) {
    if (!b) return;
    free(b->node); free(b->tri); free(b->order); free(b);
}

/* Where the ray enters node n's box before tmax, or -1 if it misses. */
static float box_enter(const BNode *n, const float o[3], const float inv[3], float tmax) {
    float t0 = 0.0f, t1 = tmax;
    for (int k = 0; k < 3; ++k) {
        float a = (n->lo[k] - o[k]) * inv[k], c = (n->hi[k] - o[k]) * inv[k];
        if (a > c) { float t = a; a = c; c = t; }
        t0 = maxf(t0, a); t1 = minf(t1, c);
        if (t0 > t1) return -1.0f;
    }
    return t0;
}

float pm_bvh_ray(const PmBvh *b, const float o[3], const float d[3], float tmin, float tmax) {
    if (!b->ntri) return -1.0f;
    float inv[3];
    for (int k = 0; k < 3; ++k) inv[k] = fabsf(d[k]) > 1e-12f ? 1.0f / d[k] : 1e12f;
    /* nearer child first, each entry kept with where the ray enters its box: the nearest hit is found sooner
     * and the boxes beyond it are skipped (the result is the same in any order) */
    uint32_t stack[64];
    float enter[64];
    int sp = 0;
    if (box_enter(&b->node[0], o, inv, tmax) < 0.0f) return -1.0f;
    stack[sp] = 0; enter[sp] = 0.0f; sp++;
    float best = tmax;
    int hit = 0;
    while (sp) {
        --sp;
        if (enter[sp] > best) continue;
        const BNode *n = &b->node[stack[sp]];
        if (n->count == 0) {
            uint32_t l = n->left, r = n->left + 1;
            float tl = box_enter(&b->node[l], o, inv, best), tr = box_enter(&b->node[r], o, inv, best);
            if (tl >= 0.0f && tr >= 0.0f && tr < tl) { uint32_t t = l; l = r; r = t; float f = tl; tl = tr; tr = f; }
            if (tr >= 0.0f && sp < 63) { stack[sp] = r; enter[sp] = tr; sp++; }
            if (tl >= 0.0f && sp < 63) { stack[sp] = l; enter[sp] = tl; sp++; }
            continue;
        }
        for (uint32_t i = n->first; i < n->first + n->count; ++i) {
            const float *t = b->tri[b->order[i]];
            float e1[3], e2[3], p[3], q[3], s[3];
            sub3(e1, t + 3, t); sub3(e2, t + 6, t);
            cross3(p, d, e2);
            float det = dot3(e1, p);
            if (fabsf(det) < 1e-14f) continue;
            float id = 1.0f / det;
            sub3(s, o, t);
            float u = dot3(s, p) * id;
            if (u < 0.0f || u > 1.0f) continue;
            cross3(q, s, e1);
            float v = dot3(d, q) * id;
            if (v < 0.0f || u + v > 1.0f) continue;
            float tt = dot3(e2, q) * id;
            if (tt > tmin && tt < best) { best = tt; hit = 1; }
        }
    }
    return hit ? best : -1.0f;
}

/* ---------------------------------------------------------------- bake --- */
float pm_reach(const PmMesh *m) {
    float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
    for (uint32_t i = 0; i < m->nv; ++i) for (int k = 0; k < 3; ++k) { lo[k] = minf(lo[k], m->v[i].p[k]); hi[k] = maxf(hi[k], m->v[i].p[k]); }
    float d[3] = { hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2] };
    return clampf(0.25f * sqrtf(dot3(d, d)), 0.02f, 0.6f);
}

/* Vertices grouped by position (quantised to 0.05 mm), across shells: the wear term sees creases. */
typedef struct Key { int32_t x, y, z; uint32_t v; } Key;
static int key_cmp(const void *a, const void *b) {
    const Key *p = (const Key *)a, *q = (const Key *)b;
    if (p->x != q->x) return p->x < q->x ? -1 : 1;
    if (p->y != q->y) return p->y < q->y ? -1 : 1;
    if (p->z != q->z) return p->z < q->z ? -1 : 1;
    return p->v < q->v ? -1 : p->v > q->v;
}
static int32_t quant(float x, float step) { return (int32_t)floorf(x / step + 0.5f); }

/* group[v]: a dense id per position (within the step), optionally per shell; returns the count. */
static uint32_t weld(const PmMesh *m, float step, int by_shell, uint32_t *group) {
    Key *k = (Key *)malloc(sizeof(Key) * (m->nv ? m->nv : 1));
    for (uint32_t i = 0; i < m->nv; ++i) {
        k[i].x = quant(m->v[i].p[0], step); k[i].y = quant(m->v[i].p[1], step);
        k[i].z = quant(m->v[i].p[2], step) ^ (by_shell ? (int32_t)m->shell[i] << 20 : 0);
        k[i].v = i;
    }
    qsort(k, m->nv, sizeof(Key), key_cmp);
    uint32_t g = 0;
    for (uint32_t i = 0; i < m->nv; ++i) {
        if (i && (k[i].x != k[i - 1].x || k[i].y != k[i - 1].y || k[i].z != k[i - 1].z)) g++;
        group[k[i].v] = g;
    }
    free(k);
    return m->nv ? g + 1 : 0;
}

void pm_bake(PmMesh *m, int rays, float reach) { pm_bake_with(m, NULL, rays, reach); }

void pm_bake_with(PmMesh *m, const PmMesh *occluders, int rays, float reach) {
    if (!m->nv || !m->ni) return;
    /* occlusion: the rays meet the mesh, and the occluders as well where given (their own shading untouched) */
    PmBvh *b;
    if (occluders && occluders->ni) {
        PmMesh all;
        memset(&all, 0, sizeof(all));
        all.nv = all.capv = m->nv + occluders->nv;
        all.ni = all.capi = m->ni + occluders->ni;
        all.v = (PmVertex *)malloc(sizeof(PmVertex) * all.nv);
        all.idx = (uint32_t *)malloc(sizeof(uint32_t) * all.ni);
        memcpy(all.v, m->v, sizeof(PmVertex) * m->nv);
        memcpy(all.v + m->nv, occluders->v, sizeof(PmVertex) * occluders->nv);
        memcpy(all.idx, m->idx, sizeof(uint32_t) * m->ni);
        for (uint32_t i = 0; i < occluders->ni; ++i) all.idx[m->ni + i] = m->nv + occluders->idx[i];
        b = pm_bvh_build(&all);
        free(all.v); free(all.idx);
    } else {
        b = pm_bvh_build(m);
    }
    for (uint32_t i = 0; i < m->nv; ++i) {
        PmVertex *v = &m->v[i];
        float n[3] = { v->n[0], v->n[1], v->n[2] }, t[3], s[3];
        float a[3] = { 1, 0, 0 };
        if (fabsf(n[0]) > 0.8f) { a[0] = 0; a[1] = 1; }
        cross3(t, n, a); norm3(t); cross3(s, n, t);
        float rot = hash_float(hash_u32(i * 2654435761u)) * 2.0f * PI_F;
        float occ = 0.0f, eps = 2e-5f + reach * 1e-4f;
        float o[3] = { v->p[0] + n[0] * eps, v->p[1] + n[1] * eps, v->p[2] + n[2] * eps };
        for (int r = 0; r < rays; ++r) {
            /* cosine-weighted, a Fibonacci spiral over the disc, turned per vertex */
            float u = ((float)r + 0.5f) / (float)rays, ph = (float)r * 2.39996323f + rot;
            float rr = sqrtf(u), ct = sqrtf(maxf(0.0f, 1.0f - u));
            float d[3];
            for (int k = 0; k < 3; ++k) d[k] = t[k] * rr * cosf(ph) + s[k] * rr * sinf(ph) + n[k] * ct;
            float h = pm_bvh_ray(b, o, d, 0.0f, reach);
            if (h > 0.0f) occ += 1.0f - 0.5f * h / reach;
        }
        float open = 1.0f - occ / (float)rays;
        v->occ = (uint8_t)(clampf(open, 0.0f, 1.0f) * 255.0f + 0.5f);
    }
    pm_bvh_free(b);
    /* wear: how far the faces round each position fall away (convex) or rise (concave) from its normal */
    uint32_t *grp = (uint32_t *)malloc(sizeof(uint32_t) * m->nv);
    uint32_t ng = weld(m, 5e-5f, 0, grp);
    uint32_t nt = m->ni / 3;
    uint32_t *cnt = (uint32_t *)calloc(ng + 1, sizeof(uint32_t)), *list = (uint32_t *)malloc(sizeof(uint32_t) * m->ni);
    for (uint32_t i = 0; i < m->ni; ++i) cnt[grp[m->idx[i]] + 1]++;
    for (uint32_t g = 0; g < ng; ++g) cnt[g + 1] += cnt[g];
    uint32_t *fill = (uint32_t *)malloc(sizeof(uint32_t) * (ng + 1));
    memcpy(fill, cnt, sizeof(uint32_t) * (ng + 1));
    for (uint32_t t = 0; t < nt; ++t) for (int k = 0; k < 3; ++k) list[fill[grp[m->idx[t * 3 + k]]]++] = t;
    /* each vertex's crease: how far the normals at its position split from its own */
    float *mind = (float *)malloc(sizeof(float) * m->nv);
    for (uint32_t i = 0; i < m->nv; ++i) {
        uint32_t g = grp[i];
        float md = 1.0f;
        for (uint32_t j = cnt[g]; j < cnt[g + 1]; ++j) {
            const uint32_t *ix = &m->idx[list[j] * 3];
            for (int k = 0; k < 3; ++k) if (grp[ix[k]] == g) md = minf(md, dot3(m->v[ix[k]].n, m->v[i].n));
        }
        mind[i] = md;
    }
    /* A crease's wear only stays at the crease where one of the vertex's faces also has a vertex off
     * it (a support loop, a bevel); a face with every corner on a crease (a plain box's, a cap) would
     * carry it right across, marking the face, not its edge. */
    uint8_t *anchored = (uint8_t *)calloc(m->nv, 1);
    for (uint32_t t = 0; t < nt; ++t) {
        const uint32_t *ix = &m->idx[t * 3];
        if (mind[ix[0]] > 0.9f || mind[ix[1]] > 0.9f || mind[ix[2]] > 0.9f)
            for (int k = 0; k < 3; ++k) anchored[ix[k]] = 1;
    }
    /* A crease wears at full strength, convex or concave by which side of the vertex's plane the faces
     * round it fall; a smooth surface by its curvature, full at a radius of 4 mm or less (a pot's wall
     * barely, a rolled rim or a bevel fully). */
    for (uint32_t i = 0; i < m->nv; ++i) {
        PmVertex *v = &m->v[i];
        uint32_t g = grp[i];
        float cvx = 0.0f, ccv = 0.0f, kcvx = 0.0f, kccv = 0.0f;
        for (uint32_t j = cnt[g]; j < cnt[g + 1]; ++j) {
            const uint32_t *ix = &m->idx[list[j] * 3];
            float c[3], d[3];
            for (int k = 0; k < 3; ++k) c[k] = (m->v[ix[0]].p[k] + m->v[ix[1]].p[k] + m->v[ix[2]].p[k]) / 3.0f;
            sub3(d, c, v->p);
            float l2 = dot3(d, d);
            if (l2 < 1e-14f) continue;
            float s = dot3(d, v->n), sn = s / sqrtf(l2);
            /* curvature only within the vertex's own smooth surface: across a crease it is the crease's */
            int own = 0;
            for (int k = 0; k < 3; ++k) if (grp[ix[k]] == g && dot3(m->v[ix[k]].n, v->n) > 0.9f) own = 1;
            if (s < 0.0f) { cvx = maxf(cvx, -sn); if (own) kcvx = maxf(kcvx, -2.0f * s / l2); }
            else { ccv = maxf(ccv, sn); if (own) kccv = maxf(kccv, 2.0f * s / l2); }
        }
        float crease = anchored[i] ? clampf((0.95f - mind[i]) / 0.25f, 0.0f, 1.0f) : 0.0f;
        float wc = crease * (cvx >= ccv ? 1.0f : -1.0f);
        float ws = clampf(0.004f * (kcvx - kccv), -1.0f, 1.0f);
        float w = fabsf(wc) > fabsf(ws) ? wc : ws;
        v->wear = (uint8_t)clampf(128.0f + 127.0f * w, 0.0f, 255.0f);
    }
    free(cnt); free(list); free(fill); free(grp); free(mind); free(anchored);
}

/* ------------------------------------------------------------ instances --- */
void pm_instances_bake(PmMesh *m, const PmInstances *l, int lod) {
    PmMesh cache[PMK_COUNT];
    uint8_t built[PMK_COUNT];
    memset(cache, 0, sizeof(cache));
    memset(built, 0, sizeof(built));
    uint16_t shell0 = 0;
    for (uint32_t i = 0; i < m->nv; ++i) if (m->shell[i] >= shell0) shell0 = (uint16_t)(m->shell[i] + 1);
    for (uint32_t k = 0; k < l->n; ++k) {
        const PmInstance *in = &l->v[k];
        if (in->module >= PMK_COUNT) continue;
        if (!built[in->module]) { pm_module_mesh(in->module, lod, &cache[in->module]); built[in->module] = 1; }
        const PmMesh *s = &cache[in->module];
        if (!s->nv) continue;
        Pm pm;
        pm_begin(&pm, m, NULL, lod);
        memcpy(pm.xf, in->xf, sizeof(pm.xf));
        uint32_t base = m->nv;
        for (uint32_t i = 0; i < s->nv; ++i) {
            pm.mat = s->v[i].mat; pm.part = in->part ? in->part : s->v[i].part;
            pm.shell = (uint16_t)(shell0 + s->shell[i]);
            uint32_t id = pm_vert(&pm, s->v[i].p, s->v[i].n, s->v[i].uv[0], s->v[i].uv[1]);
            m->v[id].occ = s->v[i].occ; m->v[id].wear = s->v[i].wear;
        }
        /* the module's own winding, mirrored if the transform mirrors */
        const float *x = in->xf;
        float c0[3] = { x[0], x[4], x[8] }, c1[3] = { x[1], x[5], x[9] }, c2[3] = { x[2], x[6], x[10] }, cc[3];
        cross3(cc, c1, c2);
        int mirror = dot3(c0, cc) < 0.0f;
        if (m->ni + s->ni > m->capi) {
            uint32_t cap = m->capi ? m->capi : 3072;
            while (cap < m->ni + s->ni) cap *= 2;
            m->idx = (uint32_t *)realloc(m->idx, sizeof(uint32_t) * cap);
            m->capi = cap;
        }
        for (uint32_t t = 0; t < s->ni; t += 3) {
            m->idx[m->ni++] = base + s->idx[t];
            m->idx[m->ni++] = base + s->idx[t + (mirror ? 2 : 1)];
            m->idx[m->ni++] = base + s->idx[t + (mirror ? 1 : 2)];
        }
        uint16_t top = 0;
        for (uint32_t i = 0; i < s->nv; ++i) if (s->shell[i] > top) top = s->shell[i];
        shell0 = (uint16_t)(shell0 + top + 1);
    }
    for (int i = 0; i < PMK_COUNT; ++i) pm_mesh_free(&cache[i]);
}

/* ---------------------------------------------------------------- stats --- */
typedef struct Edge { uint32_t a, b; int8_t dir; } Edge;
static int edge_cmp(const void *x, const void *y) {
    const Edge *p = (const Edge *)x, *q = (const Edge *)y;
    if (p->a != q->a) return p->a < q->a ? -1 : 1;
    if (p->b != q->b) return p->b < q->b ? -1 : 1;
    return p->dir - q->dir;
}

void pm_stats(const PmMesh *m, PmStats *s) {
    memset(s, 0, sizeof(*s));
    s->tris = (int)(m->ni / 3); s->verts = (int)m->nv;
    for (int k = 0; k < 3; ++k) { s->lo[k] = 1e30f; s->hi[k] = -1e30f; }
    for (uint32_t i = 0; i < m->nv; ++i) for (int k = 0; k < 3; ++k) { s->lo[k] = minf(s->lo[k], m->v[i].p[k]); s->hi[k] = maxf(s->hi[k], m->v[i].p[k]); }
    /* the hash: positions, normals and uvs to 0.01 mm, and the bytes */
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < m->nv; ++i) {
        const PmVertex *v = &m->v[i];
        for (int k = 0; k < 3; ++k) h = hash_combine(h, (uint32_t)quant(v->p[k], 1e-5f));
        for (int k = 0; k < 3; ++k) h = hash_combine(h, (uint32_t)quant(v->n[k], 1e-4f));
        for (int k = 0; k < 2; ++k) h = hash_combine(h, (uint32_t)quant(v->uv[k], 1e-5f));
        h = hash_combine(h, (uint32_t)v->mat | (uint32_t)v->occ << 8 | (uint32_t)v->wear << 16 | (uint32_t)v->part << 24);
    }
    for (uint32_t i = 0; i < m->ni; ++i) h = hash_combine(h, m->idx[i]);
    s->hash = h;
    if (!m->ni) return;
    /* closure, per shell */
    uint32_t *grp = (uint32_t *)malloc(sizeof(uint32_t) * m->nv);
    weld(m, 2e-5f, 1, grp);
    uint32_t nt = m->ni / 3;
    Edge *e = (Edge *)malloc(sizeof(Edge) * m->ni);
    uint32_t ne = 0;
    for (uint32_t t = 0; t < nt; ++t) {
        uint32_t g[3] = { grp[m->idx[t * 3]], grp[m->idx[t * 3 + 1]], grp[m->idx[t * 3 + 2]] };
        if (g[0] == g[1] || g[1] == g[2] || g[0] == g[2]) { s->degenerate++; continue; }
        for (int k = 0; k < 3; ++k) {
            uint32_t a = g[k], b = g[(k + 1) % 3];
            e[ne].a = a < b ? a : b; e[ne].b = a < b ? b : a; e[ne].dir = a < b ? 1 : -1; ne++;
        }
    }
    qsort(e, ne, sizeof(Edge), edge_cmp);
    for (uint32_t i = 0; i < ne;) {
        uint32_t j = i;
        int sum = 0;
        while (j < ne && e[j].a == e[i].a && e[j].b == e[i].b) { sum += e[j].dir; j++; }
        if (j - i != 2 || sum != 0) s->open_edges++;
        i = j;
    }
    free(e);
    free(grp);
    /* normals against the faces using them */
    for (uint32_t t = 0; t < nt; ++t) {
        const uint32_t *ix = &m->idx[t * 3];
        float e1[3], e2[3], f[3];
        sub3(e1, m->v[ix[1]].p, m->v[ix[0]].p); sub3(e2, m->v[ix[2]].p, m->v[ix[0]].p); cross3(f, e1, e2);
        if (!norm3(f)) continue;
        for (int k = 0; k < 3; ++k) if (dot3(f, m->v[ix[k]].n) < 0.1f) { s->bad_normals++; break; }
    }
    /* closed shells' volume */
    uint16_t maxs = 0;
    for (uint32_t i = 0; i < m->nv; ++i) if (m->shell[i] > maxs) maxs = m->shell[i];
    double *vol = (double *)calloc((size_t)maxs + 1, sizeof(double));
    float c[3] = { 0.5f * (s->lo[0] + s->hi[0]), 0.5f * (s->lo[1] + s->hi[1]), 0.5f * (s->lo[2] + s->hi[2]) };
    for (uint32_t t = 0; t < nt; ++t) {
        const uint32_t *ix = &m->idx[t * 3];
        float a[3], b[3], d[3], x[3];
        sub3(a, m->v[ix[0]].p, c); sub3(b, m->v[ix[1]].p, c); sub3(d, m->v[ix[2]].p, c);
        cross3(x, b, d);
        vol[m->shell[ix[0]]] += (double)dot3(a, x) / 6.0;
    }
    for (uint32_t k = 0; k <= maxs; ++k) if (vol[k] < -1e-12) s->inverted++;
    free(vol);
}
