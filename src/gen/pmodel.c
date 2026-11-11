#include "pmodel.h"
#include "mathlib.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ===========================================================================
 * The kit's core (pmodel.h): the mesh, the transform stack, emission, and the
 * primitives every part and module is made from.
 * ========================================================================= */

/* ------------------------------------------------------------- vectors --- */
static void v3set(float o[3], float x, float y, float z) { o[0] = x; o[1] = y; o[2] = z; }
static void v3sub(float o[3], const float a[3], const float b[3]) { o[0] = a[0] - b[0]; o[1] = a[1] - b[1]; o[2] = a[2] - b[2]; }
static float v3dot(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static void v3cross(float o[3], const float a[3], const float b[3]) {
    float x = a[1] * b[2] - a[2] * b[1], y = a[2] * b[0] - a[0] * b[2], z = a[0] * b[1] - a[1] * b[0];
    o[0] = x; o[1] = y; o[2] = z;
}
static float v3len(const float a[3]) { return sqrtf(v3dot(a, a)); }
static int v3norm(float a[3]) {
    float l = v3len(a);
    if (l < 1e-12f) return 0;
    a[0] /= l; a[1] /= l; a[2] /= l;
    return 1;
}
/* Rotates v about unit axis k by angle a (Rodrigues). */
static void v3rot(float o[3], const float v[3], const float k[3], float a) {
    float c = cosf(a), s = sinf(a), kv[3], d = v3dot(k, v);
    v3cross(kv, k, v);
    for (int i = 0; i < 3; ++i) o[i] = v[i] * c + kv[i] * s + k[i] * d * (1.0f - c);
}

/* ---------------------------------------------------------------- mesh --- */
static void grow_v(PmMesh *m) {
    if (m->nv < m->capv) return;
    uint32_t cap = m->capv ? m->capv * 2 : 1024;
    m->v = (PmVertex *)realloc(m->v, sizeof(PmVertex) * cap);
    m->shell = (uint16_t *)realloc(m->shell, sizeof(uint16_t) * cap);
    m->capv = cap;
}
static void grow_i(PmMesh *m, uint32_t extra) {
    if (m->ni + extra <= m->capi) return;
    uint32_t cap = m->capi ? m->capi : 3072;
    while (cap < m->ni + extra) cap *= 2;
    m->idx = (uint32_t *)realloc(m->idx, sizeof(uint32_t) * cap);
    m->capi = cap;
}

void pm_mesh_free(PmMesh *m) {
    free(m->v); free(m->idx); free(m->shell);
    memset(m, 0, sizeof(*m));
}
void pm_inst_free(PmInstances *l) { free(l->v); memset(l, 0, sizeof(*l)); }

/* ---------------------------------------------------------- transforms --- */
static void xf_ident(float m[12]) { memset(m, 0, sizeof(float) * 12); m[0] = m[5] = m[10] = 1.0f; }
/* r = a b (b applied first). */
static void xf_mul(float r[12], const float a[12], const float b[12]) {
    float o[12];
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) o[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j] + a[i * 4 + 2] * b[8 + j];
        o[i * 4 + 3] = a[i * 4] * b[3] + a[i * 4 + 1] * b[7] + a[i * 4 + 2] * b[11] + a[i * 4 + 3];
    }
    memcpy(r, o, sizeof(o));
}
static void apply(Pm *pm, const float l[12]) { xf_mul(pm->xf, pm->xf, l); }

void pm_begin(Pm *pm, PmMesh *out, PmInstances *inst, int lod) {
    memset(pm, 0, sizeof(*pm));
    pm->out = out; pm->inst = inst; pm->lod = lod;
    xf_ident(pm->xf);
    pm->mat = PMAT_STEEL;
    /* support loops keep occlusion and wear at the creases they belong to, rather than smeared
     * over whole faces between them; long profile edges are split for the same reason */
    pm->support = lod == PM_LOD0 ? 0.004f : lod == PM_LOD1 ? 0.008f : 0.0f;
    pm->maxlen = lod == PM_LOD0 ? 0.12f : lod == PM_LOD1 ? 0.3f : 0.0f;
}
void pm_push(Pm *pm) { if (pm->depth < PM_STACK) memcpy(pm->stack[pm->depth], pm->xf, sizeof(pm->xf)); pm->depth++; }
void pm_pop(Pm *pm) { if (pm->depth > 0 && --pm->depth < PM_STACK) memcpy(pm->xf, pm->stack[pm->depth], sizeof(pm->xf)); }
void pm_translate(Pm *pm, float x, float y, float z) { float l[12]; xf_ident(l); l[3] = x; l[7] = y; l[11] = z; apply(pm, l); }
void pm_rotate_x(Pm *pm, float a) { float l[12], c = cosf(a), s = sinf(a); xf_ident(l); l[5] = c; l[6] = -s; l[9] = s; l[10] = c; apply(pm, l); }
void pm_rotate_y(Pm *pm, float a) { float l[12], c = cosf(a), s = sinf(a); xf_ident(l); l[0] = c; l[2] = s; l[8] = -s; l[10] = c; apply(pm, l); }
void pm_rotate_z(Pm *pm, float a) { float l[12], c = cosf(a), s = sinf(a); xf_ident(l); l[0] = c; l[1] = -s; l[4] = s; l[5] = c; apply(pm, l); }
void pm_scale(Pm *pm, float sx, float sy, float sz) { float l[12]; xf_ident(l); l[0] = sx; l[5] = sy; l[10] = sz; apply(pm, l); }
void pm_frame(Pm *pm, const float o[3], const float ex[3], const float ey[3], const float ez[3]) {
    float l[12] = { ex[0], ey[0], ez[0], o[0], ex[1], ey[1], ez[1], o[1], ex[2], ey[2], ez[2], o[2] };
    apply(pm, l);
}
void pm_aim_y(Pm *pm, const float a[3], const float b[3], const float *side) {
    float ey[3], ex[3], ez[3];
    v3sub(ey, b, a);
    if (!v3norm(ey)) v3set(ey, 0, 1, 0);
    float s[3];
    if (side) memcpy(s, side, sizeof(s));
    else if (fabsf(ey[0]) < 0.9f) v3set(s, 1, 0, 0);
    else v3set(s, 0, 0, 1);
    float d = v3dot(s, ey);
    for (int i = 0; i < 3; ++i) ex[i] = s[i] - d * ey[i];
    if (!v3norm(ex)) { v3set(s, 0, 0, 1); d = v3dot(s, ey); for (int i = 0; i < 3; ++i) ex[i] = s[i] - d * ey[i]; v3norm(ex); }
    v3cross(ez, ex, ey);
    pm_frame(pm, a, ex, ey, ez);
}

/* ------------------------------------------------------------ emission --- */
uint32_t pm_vert(Pm *pm, const float p[3], const float n[3], float u, float v) {
    PmMesh *m = pm->out;
    grow_v(m);
    const float *x = pm->xf;
    PmVertex *o = &m->v[m->nv];
    for (int i = 0; i < 3; ++i) o->p[i] = x[i * 4] * p[0] + x[i * 4 + 1] * p[1] + x[i * 4 + 2] * p[2] + x[i * 4 + 3];
    /* normals by the cofactor of the linear part (scales and mirrors) */
    float c0[3] = { x[0], x[4], x[8] }, c1[3] = { x[1], x[5], x[9] }, c2[3] = { x[2], x[6], x[10] };
    float a[3], b[3], c[3];
    v3cross(a, c1, c2); v3cross(b, c2, c0); v3cross(c, c0, c1);
    float det = v3dot(c0, a), sg = det < 0.0f ? -1.0f : 1.0f;
    for (int i = 0; i < 3; ++i) o->n[i] = sg * (n[0] * a[i] + n[1] * b[i] + n[2] * c[i]);
    if (!v3norm(o->n)) v3set(o->n, 0, 1, 0);
    o->uv[0] = u; o->uv[1] = v;
    o->mat = pm->mat; o->part = pm->part; o->occ = 255; o->wear = 128;
    m->shell[m->nv] = pm->shell;
    return m->nv++;
}

void pm_tri(Pm *pm, uint32_t a, uint32_t b, uint32_t c) {
    PmMesh *m = pm->out;
    const float *pa = m->v[a].p, *pb = m->v[b].p, *pc = m->v[c].p;
    float e1[3], e2[3], f[3];
    v3sub(e1, pb, pa); v3sub(e2, pc, pa); v3cross(f, e1, e2);
    if (v3dot(f, f) < 1e-20f) return;
    float s[3] = { m->v[a].n[0] + m->v[b].n[0] + m->v[c].n[0], m->v[a].n[1] + m->v[b].n[1] + m->v[c].n[1],
                   m->v[a].n[2] + m->v[b].n[2] + m->v[c].n[2] };
    grow_i(m, 3);
    m->idx[m->ni++] = a;
    if (v3dot(f, s) < 0.0f) { m->idx[m->ni++] = c; m->idx[m->ni++] = b; }
    else { m->idx[m->ni++] = b; m->idx[m->ni++] = c; }
}
void pm_quad(Pm *pm, uint32_t a, uint32_t b, uint32_t c, uint32_t d) { pm_tri(pm, a, b, c); pm_tri(pm, a, c, d); }

void pm_flatten(Pm *pm, uint32_t v0, uint32_t i0) {
    PmMesh *m = pm->out;
    uint32_t nt = (m->ni - i0) / 3;
    if (!nt) return;
    PmVertex *tmp = (PmVertex *)malloc(sizeof(PmVertex) * nt * 3);
    uint16_t sh = m->nv > v0 ? m->shell[v0] : pm->shell;
    for (uint32_t t = 0; t < nt; ++t) {
        const uint32_t *ix = &m->idx[i0 + t * 3];
        float e1[3], e2[3], f[3];
        v3sub(e1, m->v[ix[1]].p, m->v[ix[0]].p); v3sub(e2, m->v[ix[2]].p, m->v[ix[0]].p); v3cross(f, e1, e2); v3norm(f);
        for (int k = 0; k < 3; ++k) { tmp[t * 3 + k] = m->v[ix[k]]; memcpy(tmp[t * 3 + k].n, f, sizeof(f)); }
    }
    m->nv = v0; m->ni = i0;
    grow_i(m, nt * 3);
    for (uint32_t k = 0; k < nt * 3; ++k) {
        grow_v(m);
        m->v[m->nv] = tmp[k];
        m->shell[m->nv] = sh;
        m->idx[m->ni++] = m->nv++;
    }
    free(tmp);
}

/* ---------------------------------------------------------- primitives --- */
int pm_segs(const Pm *pm, int base, int min) {
    int s = pm->lod == PM_LOD0 ? base : pm->lod == PM_LOD1 ? base * 2 / 3 : base / 3;
    return s < min ? min : s;
}

void pm_box(Pm *pm, float x0, float y0, float z0, float x1, float y1, float z1) {
    pm->shell++;
    static const int F[6][4][3] = {   /* corners (0: lo, 1: hi) per face */
        { { 1, 0, 0 }, { 1, 1, 0 }, { 1, 1, 1 }, { 1, 0, 1 } }, { { 0, 0, 1 }, { 0, 1, 1 }, { 0, 1, 0 }, { 0, 0, 0 } },
        { { 0, 1, 0 }, { 0, 1, 1 }, { 1, 1, 1 }, { 1, 1, 0 } }, { { 0, 0, 1 }, { 0, 0, 0 }, { 1, 0, 0 }, { 1, 0, 1 } },
        { { 1, 0, 1 }, { 1, 1, 1 }, { 0, 1, 1 }, { 0, 0, 1 } }, { { 0, 0, 0 }, { 0, 1, 0 }, { 1, 1, 0 }, { 1, 0, 0 } } };
    static const float N[6][3] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
    float lo[3] = { x0, y0, z0 }, hi[3] = { x1, y1, z1 };
    for (int f = 0; f < 6; ++f) {
        int ua = f < 2 ? 2 : 0, va = f < 2 ? 1 : f < 4 ? 2 : 1;
        uint32_t id[4];
        for (int k = 0; k < 4; ++k) {
            float p[3];
            for (int i = 0; i < 3; ++i) p[i] = F[f][k][i] ? hi[i] : lo[i];
            id[k] = pm_vert(pm, p, N[f], p[ua], p[va]);
        }
        pm_quad(pm, id[0], id[1], id[2], id[3]);
    }
}

/* Builds the ring entries of a profile or section: each point once, or twice at a crease (the end
 * of the edge before and the start of the edge after). e: (x, y, nx, ny, length, point). */
typedef struct Ent { float x, y, nx, ny, len; int pt; } Ent;
static int entries(const float (*p)[2], const uint8_t *hard, int n, int closed, float flip, Ent *e, int cap) {
    float en[512][2], el[512];
    int ne = closed ? n : n - 1;
    for (int i = 0; i < ne; ++i) {
        const float *a = p[i], *b = p[(i + 1) % n];
        float dx = b[0] - a[0], dy = b[1] - a[1], l = sqrtf(dx * dx + dy * dy);
        el[i] = l;
        en[i][0] = l > 1e-9f ? flip * dy / l : 0.0f; en[i][1] = l > 1e-9f ? -flip * dx / l : 0.0f;
    }
    int k = 0;
    float len = 0.0f;
    for (int i = 0; i < n + (closed ? 1 : 0); ++i) {
        int pi = i % n;
        const float *ni, *no;
        if (closed) { ni = en[(pi - 1 + ne) % ne]; no = en[pi]; }
        else { ni = i > 0 ? en[i - 1] : NULL; no = i < n - 1 ? en[i] : NULL; }
        if (i > 0) len += el[(i - 1) % ne];
        int crease = ni && no && (hard[pi] || ni[0] * no[0] + ni[1] * no[1] < 0.766f);   /* 40 degrees */
        float sx = (ni ? ni[0] : 0.0f) + (no ? no[0] : 0.0f), sy = (ni ? ni[1] : 0.0f) + (no ? no[1] : 0.0f);
        float sl = sqrtf(sx * sx + sy * sy);
        if (sl > 1e-9f) { sx /= sl; sy /= sl; } else { sx = no ? no[0] : ni[0]; sy = no ? no[1] : ni[1]; }
        if (closed && (i == 0 || i == n)) {
            /* the seam: the start takes the edge after, the end the edge before */
            const float *nn = i == 0 ? no : ni;
            e[k].x = p[pi][0]; e[k].y = p[pi][1]; e[k].nx = crease ? nn[0] : sx; e[k].ny = crease ? nn[1] : sy;
            e[k].len = len; e[k].pt = i; k++;
        } else if (crease) {
            e[k].x = p[pi][0]; e[k].y = p[pi][1]; e[k].nx = ni[0]; e[k].ny = ni[1]; e[k].len = len; e[k].pt = i; k++;
            e[k].x = p[pi][0]; e[k].y = p[pi][1]; e[k].nx = no[0]; e[k].ny = no[1]; e[k].len = len; e[k].pt = i; k++;
        } else {
            e[k].x = p[pi][0]; e[k].y = p[pi][1]; e[k].nx = sx; e[k].ny = sy; e[k].len = len; e[k].pt = i; k++;
        }
        if (k >= cap - 2) break;
    }
    return k;
}

/* Inserts support points beside each crease (at sup, at most a quarter of the edge, on edges at
 * least 3 sup long) and splits edges longer than maxlen; returns the new count. */
static int refine2d(const float (*p)[2], const uint8_t *hard, int n, int closed, float sup, float maxlen,
                    float (*op)[2], uint8_t *oh, int cap) {
    uint8_t crease[256];
    if (n > 256) n = 256;
    for (int i = 0; i < n; ++i) {
        crease[i] = 0;
        if (!closed && (i == 0 || i == n - 1)) continue;
        const float *a = p[(i - 1 + n) % n], *b = p[i], *c = p[(i + 1) % n];
        float d1[2] = { b[0] - a[0], b[1] - a[1] }, d2[2] = { c[0] - b[0], c[1] - b[1] };
        float l1 = sqrtf(d1[0] * d1[0] + d1[1] * d1[1]), l2 = sqrtf(d2[0] * d2[0] + d2[1] * d2[1]);
        if (l1 < 1e-9f || l2 < 1e-9f) continue;
        crease[i] = hard[i] || (d1[0] * d2[0] + d1[1] * d2[1]) / (l1 * l2) < 0.766f;
    }
    int k = 0, ne = closed ? n : n - 1;
#define EMIT(x, y, h) do { if (k < cap) { op[k][0] = (x); op[k][1] = (y); oh[k] = (uint8_t)(h); k++; } } while (0)
    for (int i = 0; i < ne; ++i) {
        const float *a = p[i], *b = p[(i + 1) % n];
        EMIT(a[0], a[1], hard[i]);
        float dx = b[0] - a[0], dy = b[1] - a[1], L = sqrtf(dx * dx + dy * dy);
        if (L < 1e-9f) continue;
        int ok = sup > 0.0f && L >= 3.0f * sup;
        float s0 = ok && crease[i] ? minf(sup, 0.25f * L) : 0.0f, s1 = ok && crease[(i + 1) % n] ? minf(sup, 0.25f * L) : 0.0f;
        float mid = L - s0 - s1;
        int nsub = maxlen > 0.0f ? (int)ceilf(mid / maxlen - 1e-4f) : 1;
        if (nsub < 1) nsub = 1;
        if (nsub > 40) nsub = 40;
        if (s0 > 0.0f) EMIT(a[0] + dx * s0 / L, a[1] + dy * s0 / L, 0);
        for (int j = 1; j < nsub; ++j) { float t = (s0 + mid * (float)j / (float)nsub) / L; EMIT(a[0] + dx * t, a[1] + dy * t, 0); }
        if (s1 > 0.0f) EMIT(b[0] - dx * s1 / L, b[1] - dy * s1 / L, 0);
    }
    if (!closed) EMIT(p[n - 1][0], p[n - 1][1], hard[n - 1]);
#undef EMIT
    return k;
}

/* Douglas-Peucker: keeps[i] for the points of p[a..b] that stray more than tol from the chord. */
static void simplify(const float (*p)[2], int a, int b, float tol, uint8_t *keep) {
    if (b - a < 2) return;
    float dx = p[b][0] - p[a][0], dy = p[b][1] - p[a][1], L = sqrtf(dx * dx + dy * dy);
    int best = -1;
    float far_ = tol;
    for (int i = a + 1; i < b; ++i) {
        float ex = p[i][0] - p[a][0], ey = p[i][1] - p[a][1];
        float d = L > 1e-9f ? fabsf(ex * dy - ey * dx) / L : sqrtf(ex * ex + ey * ey);
        if (d > far_) { far_ = d; best = i; }
    }
    if (best < 0) return;
    keep[best] = 1;
    simplify(p, a, best, tol, keep);
    simplify(p, best, b, tol, keep);
}

void pm_lathe(Pm *pm, const PmProf *prof, int n, int segs, float hx, float hz) {
    if (n < 2 || n > 120) return;
    pm->shell++;
    float p0[120][2]; uint8_t hard0[120];
    for (int i = 0; i < n; ++i) { p0[i][0] = prof[i].r; p0[i][1] = prof[i].y; hard0[i] = prof[i].hard; }
    /* the far levels drop the profile's small detail */
    float tol = pm->lod == PM_LOD1 ? 0.0005f : pm->lod == PM_LOD2 ? 0.002f : 0.0f;
    if (tol > 0.0f && n > 2) {
        uint8_t keep[120] = { 0 };
        keep[0] = keep[n - 1] = 1;
        /* hard points stay (a thin band's corners are its thickness), and split the runs between them */
        int a = 0;
        for (int i = 1; i < n; ++i)
            if (hard0[i] || i == n - 1) { keep[i] = 1; simplify((const float (*)[2])p0, a, i, tol, keep); a = i; }
        int k = 0;
        for (int i = 0; i < n; ++i) if (keep[i]) { p0[k][0] = p0[i][0]; p0[k][1] = p0[i][1]; hard0[k] = hard0[i]; k++; }
        n = k;
    }
    float p[240][2]; uint8_t hard[240];
    n = refine2d((const float (*)[2])p0, hard0, n, 0, pm->support, pm->maxlen, p, hard, 240);
    /* winding: the profile closed along the axis should be counter-clockwise in (r, y) */
    float area = 0.0f;
    for (int i = 0; i < n; ++i) { int j = (i + 1) % n; area += p[i][0] * p[j][1] - p[j][0] * p[i][1]; }
    Ent e[490];
    int ne = entries((const float (*)[2])p, hard, n, 0, area < 0.0f ? -1.0f : 1.0f, e, 490);
    /* round: a circle, or a rounded rectangle's corners */
    int round_ = hx <= 0.0f && hz <= 0.0f;
    int K;
    float B[400][2], D[400][2];
    if (segs < 3) segs = 3;
    if (round_) {
        if (segs > 256) segs = 256;
        K = segs + 1;
        for (int k = 0; k < K; ++k) { float a = 2.0f * PI_F * (float)k / (float)segs; B[k][0] = B[k][1] = 0.0f; D[k][0] = cosf(a); D[k][1] = sinf(a); }
    } else {
        int sc = segs / 4 < 1 ? 1 : segs / 4;
        if (sc > 60) sc = 60;
        static const float C[4][2] = { { 1, 1 }, { -1, 1 }, { -1, -1 }, { 1, -1 } };
        K = 0;
        for (int c = 0; c < 4; ++c) {
            for (int s = 0; s <= sc; ++s) {
                float a = 0.5f * PI_F * ((float)c + (float)s / (float)sc);
                B[K][0] = C[c][0] * hx; B[K][1] = C[c][1] * hz; D[K][0] = cosf(a); D[K][1] = sinf(a); K++;
            }
            /* the straight to the next corner: support beside each corner, split by maxlen */
            int c2 = (c + 1) & 3;
            float bx = C[c2][0] * hx - C[c][0] * hx, bz = C[c2][1] * hz - C[c][1] * hz, L = sqrtf(bx * bx + bz * bz);
            float dx = D[K - 1][0], dz = D[K - 1][1];
            if (L < 1e-6f) continue;
            float t[48];
            int nt = 0;
            float sp = pm->support > 0.0f && L >= 3.0f * pm->support ? minf(pm->support, 0.25f * L) : 0.0f;
            float mid = L - 2.0f * sp;
            int nsub = pm->maxlen > 0.0f ? (int)ceilf(mid / pm->maxlen - 1e-4f) : 1;
            if (nsub < 1) nsub = 1;
            if (nsub > 40) nsub = 40;
            if (sp > 0.0f) t[nt++] = sp / L;
            for (int j = 1; j < nsub; ++j) t[nt++] = (sp + mid * (float)j / (float)nsub) / L;
            if (sp > 0.0f) t[nt++] = 1.0f - sp / L;
            for (int j = 0; j < nt && K < 390; ++j) {
                B[K][0] = C[c][0] * hx + bx * t[j]; B[K][1] = C[c][1] * hz + bz * t[j]; D[K][0] = dx; D[K][1] = dz; K++;
            }
        }
        B[K][0] = B[0][0]; B[K][1] = B[0][1]; D[K][0] = D[0][0]; D[K][1] = D[0][1]; K++;
    }
    PmMesh *m = pm->out;
    uint32_t base = m->nv;
    for (int r = 0; r < ne; ++r) {
        float u = 0.0f, prev[3] = { 0 };
        for (int k = 0; k < K; ++k) {
            float pos[3] = { B[k][0] + e[r].x * D[k][0], e[r].y, B[k][1] + e[r].x * D[k][1] };
            if (k) { float d[3]; v3sub(d, pos, prev); u += v3len(d); }
            memcpy(prev, pos, sizeof(prev));
            float nn[3] = { e[r].nx * D[k][0], e[r].ny, e[r].nx * D[k][1] };
            pm_vert(pm, pos, nn, u, e[r].len);
        }
    }
    for (int r = 0; r + 1 < ne; ++r) {
        if (e[r].pt == e[r + 1].pt) continue;
        for (int k = 0; k + 1 < K; ++k) {
            uint32_t a = base + (uint32_t)(r * K + k), b = a + 1, c = a + (uint32_t)K + 1, d = a + (uint32_t)K;
            pm_quad(pm, a, b, c, d);
        }
    }
    /* flat caps where a rounded rectangle's profile ends on its axis */
    if (!round_)
        for (int end = 0; end < 2; ++end) {
            int pi = end ? n - 1 : 0;
            if (p[pi][0] > 1e-6f) continue;
            float y = p[pi][1], ny = end ? 1.0f : -1.0f;
            /* which way the cap faces: away from the neighbouring point */
            float other = p[end ? n - 2 : 1][1];
            if (other > y) ny = -1.0f; else if (other < y) ny = 1.0f;
            float nn[3] = { 0, ny, 0 }, cc[3] = { 0, y, 0 };
            uint32_t ctr = pm_vert(pm, cc, nn, 0.0f, 0.0f), first = m->nv;
            for (int k = 0; k < K; ++k) { float pos[3] = { B[k][0], y, B[k][1] }; pm_vert(pm, pos, nn, pos[0], pos[2]); }
            /* a support ring just inside the rim */
            float in = pm->support > 0.0f ? minf(pm->support, 0.25f * minf(hx, hz)) : 0.0f;
            if (in > 0.0f) {
                uint32_t inner = m->nv;
                /* the rectangle shrunk by in (corners' points gather at its corners) */
                for (int k = 0; k < K; ++k) {
                    float pos[3] = { clampf(B[k][0], -(hx - in), hx - in), y, clampf(B[k][1], -(hz - in), hz - in) };
                    pm_vert(pm, pos, nn, pos[0], pos[2]);
                }
                for (int k = 0; k + 1 < K; ++k) {
                    pm_tri(pm, ctr, inner + (uint32_t)k, inner + (uint32_t)k + 1);
                    pm_quad(pm, inner + (uint32_t)k, first + (uint32_t)k, first + (uint32_t)k + 1, inner + (uint32_t)k + 1);
                }
            } else
                for (int k = 0; k + 1 < K; ++k) pm_tri(pm, ctr, first + (uint32_t)k, first + (uint32_t)k + 1);
        }
}

int pm_prof_shell(const PmProf *o, int n, float t, float rim_r, int rim_segs, PmProf *out, int cap) {
    if (n < 2 || 2 * n + rim_segs + 2 > cap) return 0;
    int k = 0;
    for (int i = 0; i < n; ++i) out[k++] = o[i];
    /* the inside: each point moved in by t along its mitred normal */
    PmProf in[120];
    if (n > 120) return 0;
    for (int i = 0; i < n; ++i) {
        float n1[2] = { 0, 0 }, n2[2] = { 0, 0 };
        if (i > 0) { float dx = o[i].r - o[i - 1].r, dy = o[i].y - o[i - 1].y, l = sqrtf(dx * dx + dy * dy); if (l > 1e-9f) { n1[0] = dy / l; n1[1] = -dx / l; } }
        if (i < n - 1) { float dx = o[i + 1].r - o[i].r, dy = o[i + 1].y - o[i].y, l = sqrtf(dx * dx + dy * dy); if (l > 1e-9f) { n2[0] = dy / l; n2[1] = -dx / l; } }
        if (i == 0) { n1[0] = n2[0]; n1[1] = n2[1]; }
        if (i == n - 1) { n2[0] = n1[0]; n2[1] = n1[1]; }
        float a[2] = { n1[0] + n2[0], n1[1] + n2[1] }, l = sqrtf(a[0] * a[0] + a[1] * a[1]);
        if (l < 1e-9f) { a[0] = n1[0]; a[1] = n1[1]; } else { a[0] /= l; a[1] /= l; }
        float c = a[0] * n1[0] + a[1] * n1[1];
        float s = t / (c > 0.3f ? c : 0.3f);
        in[i].r = o[i].r - a[0] * s; in[i].y = o[i].y - a[1] * s; in[i].hard = o[i].hard;
        if (in[i].r < 0.0f || i == 0) in[i].r = o[i].r <= 1e-6f ? 0.0f : maxf(in[i].r, 0.0f);
    }
    /* the rim: a bead of radius max(rim_r, t/2) sitting on the wall's top, or a half round */
    float R = maxf(rim_r, 0.5f * t);
    float ox = o[n - 1].r, oy = o[n - 1].y, ix = in[n - 1].r, iy = in[n - 1].y;
    float mx = 0.5f * (ox + ix), my = 0.5f * (oy + iy);
    float hw = 0.5f * sqrtf((ox - ix) * (ox - ix) + (oy - iy) * (oy - iy));
    float dd = R > hw ? sqrtf(R * R - hw * hw) : 0.0f;
    /* the wall's direction at the top (outward along the rim) */
    float wx = o[n - 1].r - o[n - 2].r, wy = o[n - 1].y - o[n - 2].y, wl = sqrtf(wx * wx + wy * wy);
    wx /= wl; wy /= wl;
    float cx = mx + wx * dd, cy = my + wy * dd;
    float a0 = atan2f(oy - cy, ox - cx), a1 = atan2f(iy - cy, ix - cx);
    while (a1 <= a0) a1 += 2.0f * PI_F;
    if (rim_segs < 1) rim_segs = 1;
    for (int s = 1; s < rim_segs; ++s) {
        float a = a0 + (a1 - a0) * (float)s / (float)rim_segs;
        out[k].r = cx + R * cosf(a); out[k].y = cy + R * sinf(a); out[k].hard = 0; k++;
    }
    for (int i = n - 1; i >= 0; --i) out[k++] = in[i];
    return k;
}

/* ---------------------------------------------------------------- boxes --- */
void pm_bbox(Pm *pm, const float c[3], const float h[3], float b, int segs) {
    float bm = minf(h[0], minf(h[1], h[2])) * 0.95f;
    if (b > bm) b = bm;
    if (pm->lod == PM_LOD1) { if (b < 0.004f) b = 0.0f; segs = segs / 2 < 1 ? 1 : segs / 2; }
    if (pm->lod == PM_LOD2) { if (b < 0.012f) b = 0.0f; segs = 1; }
    if (b <= 0.0f) { pm_box(pm, c[0] - h[0], c[1] - h[1], c[2] - h[2], c[0] + h[0], c[1] + h[1], c[2] + h[2]); return; }
    if (segs < 1) segs = 1;
    if (segs > 8) segs = 8;
    PmProf pr[20];
    int n = 0;
    pr[n].r = 0; pr[n].y = -h[1]; pr[n].hard = 0; n++;
    for (int s = 0; s <= segs; ++s) { float a = -0.5f * PI_F + 0.5f * PI_F * (float)s / (float)segs; pr[n].r = b * cosf(a); pr[n].y = -h[1] + b + b * sinf(a); pr[n].hard = 0; n++; }
    for (int s = 0; s <= segs; ++s) { float a = 0.5f * PI_F * (float)s / (float)segs; pr[n].r = b * cosf(a); pr[n].y = h[1] - b + b * sinf(a); pr[n].hard = 0; n++; }
    /* drop the duplicate end points (the arc's ends sit on the axis' ends) */
    PmProf q[20]; int m = 0;
    for (int i = 0; i < n; ++i) { if (m && fabsf(q[m - 1].r - pr[i].r) < 1e-7f && fabsf(q[m - 1].y - pr[i].y) < 1e-7f) continue; q[m++] = pr[i]; }
    uint32_t v0 = pm->out->nv, i0 = pm->out->ni;
    pm_push(pm);
    pm_translate(pm, c[0], c[1], c[2]);
    pm_lathe(pm, q, m, 4 * segs, h[0] - b, h[2] - b);
    pm_pop(pm);
    if (segs == 1) pm_flatten(pm, v0, i0);
}

void pm_beam(Pm *pm, const float a[3], const float b[3], float w, float h, float bevel, const float *up) {
    float dz[3], dx[3], dy[3], u[3];
    v3sub(dz, b, a);
    float len = v3len(dz);
    if (len < 1e-6f) return;
    v3norm(dz);
    if (up) memcpy(u, up, sizeof(u)); else v3set(u, 0, 1, 0);
    if (fabsf(v3dot(u, dz)) > 0.95f * v3len(u)) v3set(u, 1, 0, 0);
    v3cross(dx, u, dz); v3norm(dx);
    v3cross(dy, dz, dx);
    float o[3] = { 0.5f * (a[0] + b[0]), 0.5f * (a[1] + b[1]), 0.5f * (a[2] + b[2]) };
    pm_push(pm);
    pm_frame(pm, o, dx, dy, dz);
    float c[3] = { 0, 0, 0 }, hh[3] = { 0.5f * w, 0.5f * h, 0.5f * len };
    pm_bbox(pm, c, hh, bevel, 1);
    pm_pop(pm);
}

/* --------------------------------------------------------------- paths --- */
void pm_path_init(PmPath *pa, int cap) {
    memset(pa, 0, sizeof(*pa));
    pa->cap = cap < 4 ? 4 : cap;
    pa->p = (float (*)[3])malloc(sizeof(float) * 3 * (size_t)pa->cap);
    pa->sharp = (uint8_t *)malloc((size_t)pa->cap);
}
void pm_path_free(PmPath *pa) { free(pa->p); free(pa->sharp); memset(pa, 0, sizeof(*pa)); }
void pm_path_add(PmPath *pa, float x, float y, float z, int sharp) {
    /* a point on top of the last adds nothing (a zero-length segment has no direction) */
    if (pa->n > 0) {
        const float *q = pa->p[pa->n - 1];
        float dx = x - q[0], dy = y - q[1], dz = z - q[2];
        if (dx * dx + dy * dy + dz * dz < 1e-12f) { pa->sharp[pa->n - 1] |= (uint8_t)sharp; return; }
    }
    if (pa->n == pa->cap) {
        pa->cap *= 2;
        pa->p = (float (*)[3])realloc(pa->p, sizeof(float) * 3 * (size_t)pa->cap);
        pa->sharp = (uint8_t *)realloc(pa->sharp, (size_t)pa->cap);
    }
    pa->p[pa->n][0] = x; pa->p[pa->n][1] = y; pa->p[pa->n][2] = z; pa->sharp[pa->n] = (uint8_t)sharp; pa->n++;
}

void pm_path_fillet(const PmPath *in, float r, int segs, PmPath *out) {
    out->n = 0; out->closed = 0;
    if (in->n < 1) return;
    pm_path_add(out, in->p[0][0], in->p[0][1], in->p[0][2], 0);
    for (int i = 1; i + 1 < in->n; ++i) {
        const float *A = in->p[i - 1], *P = in->p[i], *Bp = in->p[i + 1];
        float d1[3], d2[3];
        v3sub(d1, P, A); v3sub(d2, Bp, P);
        float l1 = v3len(d1), l2 = v3len(d2);
        if (l1 < 1e-6f || l2 < 1e-6f) continue;
        v3norm(d1); v3norm(d2);
        float c = clampf(v3dot(d1, d2), -1.0f, 1.0f), th = acosf(c);
        if (th < 1e-3f || r <= 0.0f) { pm_path_add(out, P[0], P[1], P[2], r <= 0.0f && th >= 1e-3f); continue; }
        float tn = tanf(0.5f * th), td = minf(r * tn, 0.5f * minf(l1, l2)), rr = td / tn;
        float t1[3] = { P[0] - d1[0] * td, P[1] - d1[1] * td, P[2] - d1[2] * td };
        float bis[3] = { d2[0] - d1[0], d2[1] - d1[1], d2[2] - d1[2] };
        v3norm(bis);
        float dc = rr / cosf(0.5f * th);
        float C[3] = { P[0] + bis[0] * dc, P[1] + bis[1] * dc, P[2] + bis[2] * dc };
        float ax[3]; v3cross(ax, d1, d2); v3norm(ax);
        float v0[3]; v3sub(v0, t1, C);
        int ns = (int)ceilf((float)segs * th / (0.5f * PI_F));
        if (ns < 1) ns = 1;
        for (int k = 0; k <= ns; ++k) {
            float v[3]; v3rot(v, v0, ax, th * (float)k / (float)ns);
            pm_path_add(out, C[0] + v[0], C[1] + v[1], C[2] + v[2], 0);
        }
    }
    if (in->n > 1) { const float *L = in->p[in->n - 1]; pm_path_add(out, L[0], L[1], L[2], 0); }
}

void pm_path_catenary(PmPath *out, const float a[3], const float b[3], float sag, int n) {
    if (n < 1) n = 1;
    for (int k = 0; k <= n; ++k) {
        float t = (float)k / (float)n;
        pm_path_add(out, lerpf(a[0], b[0], t), lerpf(a[1], b[1], t) - 4.0f * sag * t * (1.0f - t), lerpf(a[2], b[2], t), 0);
    }
}

void pm_path_arc(PmPath *out, const float c[3], const float ex[3], const float ey[3], float r, float a0, float a1, int n) {
    if (n < 1) n = 1;
    for (int k = 0; k <= n; ++k) {
        float a = lerpf(a0, a1, (float)k / (float)n), ca = cosf(a) * r, sa = sinf(a) * r;
        pm_path_add(out, c[0] + ex[0] * ca + ey[0] * sa, c[1] + ex[1] * ca + ey[1] * sa, c[2] + ex[2] * ca + ey[2] * sa, 0);
    }
}

/* ------------------------------------------------------------- sections --- */
void pm_sect_circle(PmSect *s, float r, int segs) {
    if (segs > 64) segs = 64;
    if (segs < 3) segs = 3;
    s->n = segs; s->closed = 1;
    for (int k = 0; k < segs; ++k) { float a = 2.0f * PI_F * (float)k / (float)segs; s->p[k][0] = r * cosf(a); s->p[k][1] = r * sinf(a); s->hard[k] = 0; }
}
void pm_sect_rect(PmSect *s, float w, float h, float bevel) {
    float x = 0.5f * w, y = 0.5f * h;
    s->closed = 1; s->n = 0;
    if (bevel <= 0.0f) {
        float P[4][2] = { { -x, -y }, { x, -y }, { x, y }, { -x, y } };
        for (int k = 0; k < 4; ++k) { s->p[k][0] = P[k][0]; s->p[k][1] = P[k][1]; s->hard[k] = 1; }
        s->n = 4;
        return;
    }
    float b = minf(bevel, 0.45f * minf(w, h));
    float P[8][2] = { { -x + b, -y }, { x - b, -y }, { x, -y + b }, { x, y - b }, { x - b, y }, { -x + b, y }, { -x, y - b }, { -x, -y + b } };
    for (int k = 0; k < 8; ++k) { s->p[k][0] = P[k][0]; s->p[k][1] = P[k][1]; s->hard[k] = 1; }
    s->n = 8;
}

/* ----------------------------------------------------------- triangulate --- */
int pm_triangulate(const float (*p)[2], int n, uint32_t *out) {
    if (n < 3 || n > 256) return 0;
    int idx[256], m = n, t = 0;
    float area = 0.0f;
    for (int i = 0; i < n; ++i) { int j = (i + 1) % n; area += p[i][0] * p[j][1] - p[j][0] * p[i][1]; }
    float sg = area < 0.0f ? -1.0f : 1.0f;
    for (int i = 0; i < n; ++i) idx[i] = i;
    int guard = 0;
    while (m > 3 && guard < 4 * n * n) {
        int found = 0;
        for (int i = 0; i < m; ++i) {
            int a = idx[(i + m - 1) % m], b = idx[i], c = idx[(i + 1) % m];
            float cr = (p[b][0] - p[a][0]) * (p[c][1] - p[a][1]) - (p[b][1] - p[a][1]) * (p[c][0] - p[a][0]);
            if (cr * sg <= 1e-12f) continue;
            int inside = 0;
            for (int k = 0; k < m && !inside; ++k) {
                int q = idx[k];
                if (q == a || q == b || q == c) continue;
                float d0 = ((p[b][0] - p[a][0]) * (p[q][1] - p[a][1]) - (p[b][1] - p[a][1]) * (p[q][0] - p[a][0])) * sg;
                float d1 = ((p[c][0] - p[b][0]) * (p[q][1] - p[b][1]) - (p[c][1] - p[b][1]) * (p[q][0] - p[b][0])) * sg;
                float d2 = ((p[a][0] - p[c][0]) * (p[q][1] - p[c][1]) - (p[a][1] - p[c][1]) * (p[q][0] - p[c][0])) * sg;
                /* on the diagonal counts as inside too: a collinear support point there would be a T-junction */
                if (d0 >= 0.0f && d1 >= 0.0f && d2 >= 0.0f) inside = 1;
            }
            if (inside) continue;
            out[t * 3] = (uint32_t)a; out[t * 3 + 1] = (uint32_t)b; out[t * 3 + 2] = (uint32_t)c; t++;
            for (int k = i; k + 1 < m; ++k) idx[k] = idx[k + 1];
            m--; found = 1;
            break;
        }
        guard++;
        if (!found) {   /* a degenerate remainder: clip the first ear regardless */
            out[t * 3] = (uint32_t)idx[m - 1]; out[t * 3 + 1] = (uint32_t)idx[0]; out[t * 3 + 2] = (uint32_t)idx[1]; t++;
            for (int k = 0; k + 1 < m; ++k) idx[k] = idx[k + 1];
            m--;
        }
    }
    if (m == 3) { out[t * 3] = (uint32_t)idx[0]; out[t * 3 + 1] = (uint32_t)idx[1]; out[t * 3 + 2] = (uint32_t)idx[2]; t++; }
    return t;
}

/* ---------------------------------------------------------------- sweep --- */
void pm_sweep(Pm *pm, const PmPath *pa_in, const PmSect *s_in, const float *up, int caps) {
    if (pa_in->n < 2 || s_in->n < 2) return;
    /* support columns beside the section's creases */
    PmSect sr;
    const PmSect *s = s_in;
    if (pm->support > 0.0f) {
        float op[160][2]; uint8_t oh[160];
        int k = refine2d((const float (*)[2])s_in->p, s_in->hard, s_in->n, s_in->closed, pm->support, 0.0f, op, oh, 160);
        if (k <= 64) {
            sr.n = k; sr.closed = s_in->closed;
            for (int i = 0; i < k; ++i) { sr.p[i][0] = op[i][0]; sr.p[i][1] = op[i][1]; sr.hard[i] = oh[i]; }
            s = &sr;
        }
    }
    /* support rings beside capped ends and sharp turns */
    PmPath rp;
    pm_path_init(&rp, pa_in->n * 3 + 4);
    rp.closed = pa_in->closed;
    {
        int n = pa_in->n, nseg_in = pa_in->closed ? n : n - 1;
        int capped = !pa_in->closed && s_in->closed;
        for (int i = 0; i < nseg_in; ++i) {
            const float *a = pa_in->p[i], *b = pa_in->p[(i + 1) % n];
            pm_path_add(&rp, a[0], a[1], a[2], pa_in->sharp[i]);
            float d[3]; v3sub(d, b, a);
            float L = v3len(d);
            if (pm->support <= 0.0f || L < 3.0f * pm->support) continue;
            int j = (i + 1) % n;
            int c0 = (i == 0 && !pa_in->closed) ? (capped && (caps & 1)) : pa_in->sharp[i];
            int c1 = (j == n - 1 && !pa_in->closed) ? (capped && (caps & 2)) : pa_in->sharp[j];
            float sp = minf(pm->support, 0.25f * L);
            if (c0) pm_path_add(&rp, a[0] + d[0] * sp / L, a[1] + d[1] * sp / L, a[2] + d[2] * sp / L, 0);
            if (c1) pm_path_add(&rp, b[0] - d[0] * sp / L, b[1] - d[1] * sp / L, b[2] - d[2] * sp / L, 0);
        }
        if (!pa_in->closed) { const float *l = pa_in->p[n - 1]; pm_path_add(&rp, l[0], l[1], l[2], pa_in->sharp[n - 1]); }
    }
    const PmPath *pa = &rp;
    int N = pa->n, closed = pa->closed;
    if (N < 2) { pm_path_free(&rp); return; }
    pm->shell++;
    int nseg = closed ? N : N - 1;
    float (*T)[3] = (float (*)[3])malloc(sizeof(float) * 3 * (size_t)(nseg + 1));
    float (*X)[3] = (float (*)[3])malloc(sizeof(float) * 3 * (size_t)(nseg + 1));
    float (*Y)[3] = (float (*)[3])malloc(sizeof(float) * 3 * (size_t)(nseg + 1));
    for (int i = 0; i < nseg; ++i) {
        v3sub(T[i], pa->p[(i + 1) % N], pa->p[i]);
        if (!v3norm(T[i])) { if (i) memcpy(T[i], T[i - 1], sizeof(T[i])); else v3set(T[i], 0, 0, 1); }
    }
    float u0[3];
    if (up) memcpy(u0, up, sizeof(u0)); else v3set(u0, 0, 1, 0);
    v3cross(X[0], T[0], u0);
    if (!v3norm(X[0])) { float alt[3] = { 1, 0, 0 }; if (fabsf(T[0][0]) > 0.9f) v3set(alt, 0, 0, 1); v3cross(X[0], T[0], alt); v3norm(X[0]); }
    v3cross(Y[0], X[0], T[0]);
    for (int i = 1; i < nseg; ++i) {
        float ax[3]; v3cross(ax, T[i - 1], T[i]);
        float sn = v3len(ax), c = v3dot(T[i - 1], T[i]);
        if (sn > 1e-7f) { for (int k = 0; k < 3; ++k) ax[k] /= sn; v3rot(X[i], X[i - 1], ax, atan2f(sn, c)); }
        else memcpy(X[i], X[i - 1], sizeof(X[i]));
        /* keep it square to the tangent */
        float d = v3dot(X[i], T[i]);
        for (int k = 0; k < 3; ++k) X[i][k] -= d * T[i][k];
        v3norm(X[i]);
        v3cross(Y[i], X[i], T[i]);
    }
    /* the section's entries (columns) */
    float area = 0.0f;
    for (int i = 0; i < s->n; ++i) { int j = (i + 1) % s->n; area += s->p[i][0] * s->p[j][1] - s->p[j][0] * s->p[i][1]; }
    if (!s->closed) area = 1.0f;
    Ent e[140];
    int ne = entries((const float (*)[2])s->p, s->hard, s->n, s->closed, area < 0.0f ? -1.0f : 1.0f, e, 140);
    /* stations (rings) */
    PmMesh *m = pm->out;
    int nst = closed ? N + 1 : N;
    uint32_t *ring = (uint32_t *)malloc(sizeof(uint32_t) * (size_t)nst * 2);
    int *rpt = (int *)malloc(sizeof(int) * (size_t)nst * 2);
    int nr = 0;
    float vlen = 0.0f;
    for (int st = 0; st < nst; ++st) {
        int i = st % N;
        if (st > 0) { float d[3]; v3sub(d, pa->p[i], pa->p[(st - 1) % N]); vlen += v3len(d); }
        int has_in = closed || st > 0, has_out = closed || st < N - 1;
        int si = (st - 1 + nseg) % nseg, so = st % nseg;
        if (!closed && st == N - 1) so = nseg - 1;
        if (!closed && st == 0) si = 0;
        const float *Ti = T[si], *To = T[so];
        float M[3];
        int mitre = has_in && has_out;
        if (mitre) { for (int k = 0; k < 3; ++k) M[k] = Ti[k] + To[k]; if (!v3norm(M)) memcpy(M, To, sizeof(M)); }
        int sharp = mitre && pa->sharp[i];
        int passes = sharp ? 2 : 1;
        for (int ps = 0; ps < passes; ++ps) {
            /* the frame for positions (incoming where there is one) and for normals */
            const float *PX = has_in ? X[si] : X[so], *PY = has_in ? Y[si] : Y[so], *PT = has_in ? Ti : To;
            float NX[3], NY[3];
            if (sharp) { memcpy(NX, ps ? X[so] : X[si], 12); memcpy(NY, ps ? Y[so] : Y[si], 12); }
            else if (mitre) { for (int k = 0; k < 3; ++k) { NX[k] = X[si][k] + X[so][k]; NY[k] = Y[si][k] + Y[so][k]; } v3norm(NX); v3norm(NY); }
            else { memcpy(NX, PX, 12); memcpy(NY, PY, 12); }
            ring[nr] = m->nv; rpt[nr] = st; nr++;
            for (int c = 0; c < ne; ++c) {
                float q[3];
                for (int k = 0; k < 3; ++k) q[k] = pa->p[i][k] + e[c].x * PX[k] + e[c].y * PY[k];
                if (mitre) {
                    float d[3]; v3sub(d, q, pa->p[i]);
                    /* the mitre's stretch, held at four times for bends near a reversal */
                    float den = maxf(v3dot(PT, M), 0.25f);
                    float sx = -v3dot(d, M) / den;
                    for (int k = 0; k < 3; ++k) q[k] += sx * PT[k];
                }
                float nn[3];
                for (int k = 0; k < 3; ++k) nn[k] = e[c].nx * NX[k] + e[c].ny * NY[k];
                pm_vert(pm, q, nn, e[c].len, vlen);
            }
        }
    }
    for (int r = 0; r + 1 < nr; ++r) {
        if (rpt[r] == rpt[r + 1]) continue;
        for (int c = 0; c + 1 < ne; ++c) {
            if (e[c].pt == e[c + 1].pt) continue;
            uint32_t a = ring[r] + (uint32_t)c, b = ring[r] + (uint32_t)c + 1, cc = ring[r + 1] + (uint32_t)c + 1, d = ring[r + 1] + (uint32_t)c;
            pm_quad(pm, a, b, cc, d);
        }
    }
    /* caps */
    if (!closed && s->closed && caps) {
        uint32_t tr[3 * 64];
        int nt = pm_triangulate((const float (*)[2])s->p, s->n, tr);
        for (int end = 0; end < 2; ++end) {
            if (!(caps & (1 << end))) continue;
            int i = end ? N - 1 : 0;
            const float *TX = end ? X[nseg - 1] : X[0], *TY = end ? Y[nseg - 1] : Y[0], *TT = end ? T[nseg - 1] : T[0];
            float nn[3] = { end ? TT[0] : -TT[0], end ? TT[1] : -TT[1], end ? TT[2] : -TT[2] };
            uint32_t first = m->nv;
            for (int k = 0; k < s->n; ++k) {
                float q[3];
                for (int j = 0; j < 3; ++j) q[j] = pa->p[i][j] + s->p[k][0] * TX[j] + s->p[k][1] * TY[j];
                pm_vert(pm, q, nn, s->p[k][0], s->p[k][1]);
            }
            for (int t = 0; t < nt; ++t) pm_tri(pm, first + tr[t * 3], first + tr[t * 3 + 1], first + tr[t * 3 + 2]);
        }
    }
    free(ring); free(rpt); free(T); free(X); free(Y);
    pm_path_free(&rp);
}

void pm_tube(Pm *pm, const PmPath *pa, float r, int segs, int caps) {
    PmSect s;
    pm_sect_circle(&s, r, pm_segs(pm, segs, 4));
    pm_sweep(pm, pa, &s, NULL, caps);
}

void pm_extrude(Pm *pm, const float (*poly)[2], int n, float z0, float z1) {
    if (n < 3 || n > 64) return;
    PmSect s;
    s.n = n; s.closed = 1;
    /* the sweep's x runs along cross(+z, y) = -x: mirror the section to keep the polygon's x */
    for (int k = 0; k < n; ++k) { s.p[k][0] = -poly[k][0]; s.p[k][1] = poly[k][1]; s.hard[k] = 0; }
    PmPath pa;
    pm_path_init(&pa, 2);
    pm_path_add(&pa, 0, 0, z0, 0);
    pm_path_add(&pa, 0, 0, z1, 0);
    float up[3] = { 0, 1, 0 };
    pm_sweep(pm, &pa, &s, up, 3);
    pm_path_free(&pa);
}

void pm_torus(Pm *pm, float R, float r, int segs, int tsegs) {
    PmPath pa;
    int n = pm_segs(pm, segs, 6);
    pm_path_init(&pa, n + 1);
    for (int k = 0; k < n; ++k) { float a = 2.0f * PI_F * (float)k / (float)n; pm_path_add(&pa, R * cosf(a), 0.0f, R * sinf(a), 0); }
    pa.closed = 1;
    PmSect s;
    pm_sect_circle(&s, r, pm_segs(pm, tsegs, 4));
    pm_sweep(pm, &pa, &s, NULL, 0);
    pm_path_free(&pa);
}

void pm_disc(Pm *pm, float y, float r, int segs) {
    pm->shell++;
    int n = pm_segs(pm, segs, 4);
    float nn[3] = { 0, 1, 0 }, c[3] = { 0, y, 0 };
    uint32_t ctr = pm_vert(pm, c, nn, 0, 0), first = pm->out->nv;
    for (int k = 0; k <= n; ++k) { float a = 2.0f * PI_F * (float)k / (float)n, p[3] = { r * cosf(a), y, r * sinf(a) }; pm_vert(pm, p, nn, p[0], p[2]); }
    for (int k = 0; k < n; ++k) pm_tri(pm, ctr, first + (uint32_t)k, first + (uint32_t)k + 1);
}

void pm_cyl(Pm *pm, float y0, float y1, float r, int segs) {
    PmProf p[4] = { { 0, y0, 0 }, { r, y0, 0 }, { r, y1, 0 }, { 0, y1, 0 } };
    pm_lathe(pm, p, 4, pm_segs(pm, segs, 4), 0, 0);
}
