#include "vendor_internal.h"
#include <string.h>

/* ===========================================================================
 * A vendor as triangles in its own frame (vendor.h). The structure's parts
 * are boxes, beams, cylinders, quads, vaults, cones and blobs; the items are
 * a few of those each. Normals are flat, from the winding, which faces out.
 * With no mesh, it only counts: that is how the budget (section 9) is
 * measured, on exactly what would be drawn.
 * ========================================================================= */

typedef struct MCtx {
    VendorMesh *m;
    int         layer;     /* -1 both */
    int         count;     /* triangles */
    uint8_t     surf, col, lay, flags, role;
    uint32_t    rgb;
    uint8_t     top;       /* the part's top, 5 cm steps (VendorVertex.top) */
} MCtx;

typedef struct V3 { float x, y, z; } V3;
static V3 p3(float x, float y, float z) { V3 r = { x, y, z }; return r; }
static V3 add3(V3 a, V3 b) { return p3(a.x + b.x, a.y + b.y, a.z + b.z); }
static V3 sub3(V3 a, V3 b) { return p3(a.x - b.x, a.y - b.y, a.z - b.z); }
static V3 mul3(V3 a, float s) { return p3(a.x * s, a.y * s, a.z * s); }
static V3 cross3(V3 a, V3 b) { return p3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
static float dot3(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static V3 norm3(V3 a) { float l = sqrtf(dot3(a, a)); return l > 1e-9f ? mul3(a, 1.0f / l) : p3(0, 1, 0); }

static int wanted(const MCtx *c) { return c->layer < 0 || c->layer == c->lay; }

static void push(MCtx *c, V3 p, V3 n, float u, float v) {
    VendorMesh *m = c->m;
    if (m->count == m->cap) {
        uint32_t cap = m->cap ? m->cap * 2 : 4096;
        VendorVertex *nv = (VendorVertex *)realloc(m->v, sizeof(VendorVertex) * cap);
        if (!nv) return;
        m->v = nv; m->cap = cap;
    }
    VendorVertex *o = &m->v[m->count++];
    o->p[0] = p.x; o->p[1] = p.y; o->p[2] = p.z;
    o->n[0] = n.x; o->n[1] = n.y; o->n[2] = n.z;
    o->uv[0] = u; o->uv[1] = v;
    o->surf = c->surf; o->col = c->col; o->layer = c->lay; o->flags = c->flags; o->rgb = c->rgb; o->role = c->role;
    o->top = c->top; o->pad[0] = o->pad[1] = 0;
}

static void tri(MCtx *c, V3 a, V3 b, V3 d, const float uv[3][2]) {
    if (!wanted(c)) return;
    c->count++;
    if (!c->m) return;
    V3 n = norm3(cross3(sub3(b, a), sub3(d, a)));
    push(c, a, n, uv[0][0], uv[0][1]);
    push(c, b, n, uv[1][0], uv[1][1]);
    push(c, d, n, uv[2][0], uv[2][1]);
}

/* A quad a b c d (wound so its normal is (b-a) x (c-a)), uv in metres along ab and ad. */
static void quad(MCtx *c, V3 a, V3 b, V3 cc, V3 d) {
    float lu = sqrtf(dot3(sub3(b, a), sub3(b, a))), lv = sqrtf(dot3(sub3(d, a), sub3(d, a)));
    float t0[3][2] = { { 0, 0 }, { lu, 0 }, { lu, lv } }, t1[3][2] = { { 0, 0 }, { lu, lv }, { 0, lv } };
    tri(c, a, b, cc, t0);
    tri(c, a, cc, d, t1);
}
/* ...wound to face `want`. */
static void quad_out(MCtx *c, V3 a, V3 b, V3 cc, V3 d, V3 want) {
    if (dot3(cross3(sub3(b, a), sub3(cc, a)), want) < 0.0f) quad(c, a, d, cc, b);
    else quad(c, a, b, cc, d);
}

/* An oriented box: centre o, right-handed axes ex ey ez, half sizes h; faces (VB_*) left out. */
static void obox(MCtx *c, V3 o, V3 ex, V3 ey, V3 ez, float hx, float hy, float hz, int skip) {
    V3 X = mul3(ex, hx), Y = mul3(ey, hy), Z = mul3(ez, hz);
#define CORNER(sx, sy, sz) add3(o, add3(mul3(X, (float)(sx)), add3(mul3(Y, (float)(sy)), mul3(Z, (float)(sz)))))
    if (!(skip & VB_NX))  quad(c, CORNER(-1, -1, -1), CORNER(-1, -1, 1), CORNER(-1, 1, 1), CORNER(-1, 1, -1));
    if (!(skip & VB_PX))  quad(c, CORNER(1, -1, 1), CORNER(1, -1, -1), CORNER(1, 1, -1), CORNER(1, 1, 1));
    if (!(skip & VB_NZ))  quad(c, CORNER(1, -1, -1), CORNER(-1, -1, -1), CORNER(-1, 1, -1), CORNER(1, 1, -1));
    if (!(skip & VB_PZ))  quad(c, CORNER(-1, -1, 1), CORNER(1, -1, 1), CORNER(1, 1, 1), CORNER(-1, 1, 1));
    if (!(skip & VB_TOP)) quad(c, CORNER(-1, 1, 1), CORNER(1, 1, 1), CORNER(1, 1, -1), CORNER(-1, 1, -1));
    if (!(skip & VB_BOT)) quad(c, CORNER(-1, -1, -1), CORNER(1, -1, -1), CORNER(1, -1, 1), CORNER(-1, -1, 1));
#undef CORNER
}

static void abox(MCtx *c, V3 a, V3 b, int skip) {
    obox(c, mul3(add3(a, b), 0.5f), p3(1, 0, 0), p3(0, 1, 0), p3(0, 0, 1), 0.5f * (b.x - a.x), 0.5f * (b.y - a.y), 0.5f * (b.z - a.z), skip);
}

/* Two axes square to d, right-handed with it (ex x ey = d). */
static void basis(V3 d, V3 *ex, V3 *ey) {
    V3 up = fabsf(d.y) < 0.9f ? p3(0, 1, 0) : p3(1, 0, 0);
    *ex = norm3(cross3(up, d));
    *ey = cross3(d, *ex);
}

static void beam(MCtx *c, V3 a, V3 b, float size) {
    V3 d = sub3(b, a);
    float len = sqrtf(dot3(d, d));
    if (len < 1e-4f) return;
    d = mul3(d, 1.0f / len);
    V3 ex, ey;
    basis(d, &ex, &ey);
    obox(c, mul3(add3(a, b), 0.5f), ex, ey, d, 0.5f * size, 0.5f * size, 0.5f * len, 0);
}

static int segs_at(int segs, int detail) {
    if (detail >= 2) return segs;
    if (detail == 1) return segs * 2 / 3 > 4 ? segs * 2 / 3 : 4;
    return segs / 2 > 3 ? segs / 2 : 3;
}

/* A cylinder from a to b; caps unless skip has VB_BOT (at a) or VB_TOP (at b). */
static void cyl(MCtx *c, V3 a, V3 b, float r, int segs, int skip) {
    V3 d = sub3(b, a);
    float len = sqrtf(dot3(d, d));
    if (len < 1e-5f || segs < 3) return;
    d = mul3(d, 1.0f / len);
    V3 ex, ey;
    basis(d, &ex, &ey);
    V3 ra[24], rb[24];
    if (segs > 24) segs = 24;
    for (int k = 0; k < segs; ++k) {
        float t = 2.0f * PI_F * (float)k / (float)segs;
        V3 o = add3(mul3(ex, r * cosf(t)), mul3(ey, r * sinf(t)));
        ra[k] = add3(a, o); rb[k] = add3(b, o);
    }
    float circ = 2.0f * PI_F * r / (float)segs;
    for (int k = 0; k < segs; ++k) {
        int k1 = (k + 1) % segs;
        float t0[3][2] = { { circ * k, 0 }, { circ * (k + 1), 0 }, { circ * (k + 1), len } }, t1[3][2] = { { circ * k, 0 }, { circ * (k + 1), len }, { circ * k, len } };
        tri(c, ra[k], ra[k1], rb[k1], t0);
        tri(c, ra[k], rb[k1], rb[k], t1);
    }
    float tz[3][2] = { { 0, 0 }, { r, 0 }, { 0, r } };
    for (int k = 1; k + 1 < segs; ++k) {
        if (!(skip & VB_TOP)) tri(c, rb[0], rb[k], rb[k + 1], tz);
        if (!(skip & VB_BOT)) tri(c, ra[0], ra[k + 1], ra[k], tz);
    }
}

static void cone(MCtx *c, V3 apex, V3 rim, float r, int segs, int twoSided) {
    if (segs > 24) segs = 24;
    V3 p[24];
    for (int k = 0; k < segs; ++k) {
        float t = 2.0f * PI_F * (float)k / (float)segs;
        p[k] = add3(rim, p3(r * cosf(t), 0.0f, r * sinf(t)));
    }
    float uv[3][2] = { { 0, 0 }, { r, 0 }, { 0, r } };
    for (int k = 0; k < segs; ++k) {
        int k1 = (k + 1) % segs;
        /* up (away from the rim's centre below the apex) first */
        V3 n = cross3(sub3(p[k1], apex), sub3(p[k], apex));
        if (n.y >= 0.0f) tri(c, apex, p[k1], p[k], uv); else tri(c, apex, p[k], p[k1], uv);
        if (twoSided) { if (n.y >= 0.0f) tri(c, apex, p[k], p[k1], uv); else tri(c, apex, p[k1], p[k], uv); }
    }
}

/* A lumpy octahedron filling the box a .. b. */
static void blob(MCtx *c, V3 a, V3 b, uint32_t seed) {
    V3 o = mul3(add3(a, b), 0.5f), h = mul3(sub3(b, a), 0.5f);
    V3 v[6];
    static const float D[6][3] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
    for (int k = 0; k < 6; ++k) {
        float j = 0.85f + 0.3f * hash_float(hash_combine(seed, (uint32_t)k));
        v[k] = add3(o, p3(D[k][0] * h.x * j, D[k][1] * h.y * (k == 3 ? 1.0f : j), D[k][2] * h.z * j));
    }
    static const int F[8][3] = { { 0, 2, 4 }, { 4, 2, 1 }, { 1, 2, 5 }, { 5, 2, 0 }, { 0, 4, 3 }, { 4, 1, 3 }, { 1, 5, 3 }, { 5, 0, 3 } };
    float uv[3][2] = { { 0, 0 }, { 1, 0 }, { 0, 1 } };
    for (int f = 0; f < 8; ++f) {
        V3 A = v[F[f][0]], B = v[F[f][1]], C = v[F[f][2]];
        V3 n = cross3(sub3(B, A), sub3(C, A)), out = sub3(mul3(add3(add3(A, B), C), 1.0f / 3.0f), o);
        if (dot3(n, out) < 0.0f) tri(c, A, C, B, uv); else tri(c, A, B, C, uv);
    }
}

static V3 qv(const float q[3]) { return p3(q[0], q[1], q[2]); }

/* A quad part, two-sided or scalloped along its lower edge (q[2], q[3]) if asked. */
static void quad_part(MCtx *c, const VPart *p, int detail) {
    V3 q0 = qv(p->q[0]), q1 = qv(p->q[1]), q2 = qv(p->q[2]), q3 = qv(p->q[3]);
    int two = (p->flags & VPF_TWOSIDED) != 0;
    if (!(p->flags & VPF_SCALLOP)) {
        quad(c, q0, q1, q2, q3);
        if (two) quad(c, q0, q3, q2, q1);
        return;
    }
    int n = segs_at(p->segs ? p->segs * 2 : 12, detail);
    float uv[3][2] = { { 0, 0 }, { 1, 0 }, { 0, 1 } };
    for (int k = 0; k < n; ++k) {
        float t0 = (float)k / (float)n, t1 = (float)(k + 1) / (float)n, tm = 0.5f * (t0 + t1);
        V3 top0 = add3(q0, mul3(sub3(q1, q0), t0)), top1 = add3(q0, mul3(sub3(q1, q0), t1));
        V3 b0 = add3(q3, mul3(sub3(q2, q3), t0)), b1 = add3(q3, mul3(sub3(q2, q3), t1)), bm = add3(q3, mul3(sub3(q2, q3), tm));
        /* the scallop's points stand 40% up the drop, its middle at the bottom */
        b0 = add3(b0, mul3(sub3(top0, b0), 0.4f)); b1 = add3(b1, mul3(sub3(top1, b1), 0.4f));
        tri(c, top0, top1, b1, uv); tri(c, top0, b1, bm, uv); tri(c, top0, bm, b0, uv);
        if (two) { tri(c, top0, b1, top1, uv); tri(c, top0, bm, b1, uv); tri(c, top0, b0, bm, uv); }
    }
}

/* A vault along x: its underside rises r over the middle of its span across z; a .. b its box. */
static void vault(MCtx *c, const VPart *p, int detail) {
    int S = segs_at(p->segs, detail);
    float x0 = p->a[0], x1 = p->b[0], z0 = p->a[2], z1 = p->b[2], y0 = p->a[1], th = p->b[1] - p->a[1];
    V3 under[25], over[25];
    if (S > 24) S = 24;
    for (int k = 0; k <= S; ++k) {
        float t = (float)k / (float)S, z = z0 + (z1 - z0) * t, y = y0 + p->r * sinf(PI_F * t);
        under[k] = p3(0, y, z); over[k] = p3(0, y + th, z);
    }
    for (int k = 0; k < S; ++k) {
        V3 dz = sub3(under[k + 1], under[k]);
        V3 up = norm3(p3(0, dz.z, -dz.y));
        if (up.y < 0) up = mul3(up, -1.0f);
        quad_out(c, p3(x0, over[k].y, over[k].z), p3(x1, over[k].y, over[k].z), p3(x1, over[k + 1].y, over[k + 1].z), p3(x0, over[k + 1].y, over[k + 1].z), up);
        quad_out(c, p3(x0, under[k].y, under[k].z), p3(x1, under[k].y, under[k].z), p3(x1, under[k + 1].y, under[k + 1].z), p3(x0, under[k + 1].y, under[k + 1].z), mul3(up, -1.0f));
        quad_out(c, p3(x0, under[k].y, under[k].z), p3(x0, under[k + 1].y, under[k + 1].z), p3(x0, over[k + 1].y, over[k + 1].z), p3(x0, over[k].y, over[k].z), p3(-1, 0, 0));
        quad_out(c, p3(x1, under[k].y, under[k].z), p3(x1, under[k + 1].y, under[k + 1].z), p3(x1, over[k + 1].y, over[k + 1].z), p3(x1, over[k].y, over[k].z), p3(1, 0, 0));
    }
    quad_out(c, p3(x0, under[0].y, z0), p3(x1, under[0].y, z0), p3(x1, over[0].y, z0), p3(x0, over[0].y, z0), p3(0, 0, -1));
    quad_out(c, p3(x0, under[S].y, z1), p3(x1, under[S].y, z1), p3(x1, over[S].y, z1), p3(x0, over[S].y, z1), p3(0, 0, 1));
}

static void set(MCtx *c, int surf, int col, int layer, int flags, int role, uint32_t rgb) {
    c->surf = (uint8_t)surf; c->col = (uint8_t)col; c->lay = (uint8_t)layer; c->flags = (uint8_t)flags; c->role = (uint8_t)role; c->rgb = rgb;
    c->top = 0;
}

static void emit_part(MCtx *c, const Vendor *v, const VPart *p) {
    int surf = p->surf == VS_FINISH ? v_finish_surf(v) : p->surf;
    set(c, surf, p->col, p->layer, p->flags, p->role, p->col == VCOL_RGB ? p->rgb : (p->seed & 0xFFFFFFu));
    if (!wanted(c)) return;
    float mn[3], mx[3];
    v_part_aabb(p, mn, mx);
    c->top = (uint8_t)clampf(mx[1] / 0.05f + 0.5f, 0.0f, 255.0f);
    switch (p->shape) {
    case VP_BOX:   abox(c, qv(p->a), qv(p->b), p->faces); break;
    case VP_BEAM:  beam(c, qv(p->a), qv(p->b), p->r); break;
    case VP_CYL:   cyl(c, qv(p->a), qv(p->b), p->r, segs_at(p->segs, v->detail), p->faces); break;
    case VP_QUAD:  quad_part(c, p, v->detail); break;
    case VP_VAULT: vault(c, p, v->detail); break;
    case VP_CONE:  cone(c, qv(p->a), qv(p->b), p->r, segs_at(p->segs, v->detail), (p->flags & VPF_TWOSIDED) != 0); break;
    case VP_BLOB:  blob(c, qv(p->a), qv(p->b), p->seed); break;
    }
}

/* ------------------------------------------------------------------ items --- */

/* An item's frame: its foot p, right ex, up, front ez (from its facing). */
typedef struct IF { V3 o, ex, ey, ez; } IF;
static IF iframe(const VItem *it) {
    IF f;
    float l = sqrtf(it->fx * it->fx + it->fz * it->fz);
    float fx = l > 1e-5f ? it->fx / l : 0.0f, fz = l > 1e-5f ? it->fz / l : 1.0f;
    f.o = p3(it->p[0], it->p[1], it->p[2]); f.ez = p3(fx, 0, fz); f.ey = p3(0, 1, 0); f.ex = p3(fz, 0, -fx);
    return f;
}
static V3 at(const IF *f, float x, float y, float z) { return add3(f->o, add3(mul3(f->ex, x), add3(mul3(f->ey, y), mul3(f->ez, z)))); }
/* a box in the item's frame: x0 .. x1 across, y0 .. y1 up, z0 .. z1 front */
static void ib(MCtx *c, const IF *f, float x0, float y0, float z0, float x1, float y1, float z1, int skip) {
    obox(c, at(f, 0.5f * (x0 + x1), 0.5f * (y0 + y1), 0.5f * (z0 + z1)), f->ex, f->ey, f->ez, 0.5f * (x1 - x0), 0.5f * (y1 - y0), 0.5f * (z1 - z0), skip);
}
static void ic(MCtx *c, const IF *f, float x, float y0, float z, float y1, float r, int segs, int skip) {
    cyl(c, at(f, x, y0, z), at(f, x, y1, z), r, segs, skip);
}
static void ibl(MCtx *c, const IF *f, float x, float y, float z, float hx, float hy, float hz, uint32_t seed) {
    V3 o = at(f, x, y, z);
    blob(c, sub3(o, p3(hx, hy, hz)), add3(o, p3(hx, hy, hz)), seed);
}

static uint32_t tone(uint32_t rgb, uint32_t seed, float spread) { return v_shade(rgb, 1.0f - spread + 2.0f * spread * hash_float(seed)); }

static void emit_item(MCtx *c, const Vendor *v, const VItem *it) {
    IF f = iframe(it);
    float w = it->s[0], h = it->s[1], d = it->s[2], hw = 0.5f * w, hd = 0.5f * d;
    int sg = segs_at(8, v->detail), sg6 = segs_at(6, v->detail);
    uint32_t s = it->seed, rgb = it->rgb ? it->rgb : 0x808080u;
    int lay = VL_DRESSING, fl = it->flags & VPF_SWAY, role = it->role;
#define SET(SURF, RGB) set(c, (SURF), VCOL_RGB, lay, fl, role, (RGB))
    if (it->sig) {
        switch (it->kind) {
        case VSIG_CAT:
            SET(VS_PROP, 0xF2EEE6u); ib(c, &f, -0.07f, 0.0f, -0.06f, 0.07f, 0.15f, 0.06f, VB_BOT);
            ibl(c, &f, 0.0f, 0.2f, 0.0f, 0.075f, 0.06f, 0.065f, s);
            SET(VS_PROP, 0xD8B040u); ib(c, &f, 0.05f, 0.14f, 0.03f, 0.09f, 0.24f, 0.06f, VB_BOT);
            return;
        case VSIG_SHRINE:
            SET(VS_PROP, 0xA81E1Eu); ib(c, &f, -hw, 0.0f, -hd, hw, h * 0.7f, hd, 0);
            SET(VS_PROP, 0x2A1A10u); ib(c, &f, -hw - 0.04f, h * 0.7f, -hd - 0.03f, hw + 0.04f, h * 0.8f, hd + 0.03f, 0);
            set(c, VS_BULB, VCOL_RGB, lay, VPF_EMISSIVE, role, 0xFF6A20u);
            ib(c, &f, -0.08f, 0.02f, hd - 0.02f, -0.04f, 0.08f, hd + 0.02f, VB_BOT);
            ib(c, &f, 0.04f, 0.02f, hd - 0.02f, 0.08f, 0.08f, hd + 0.02f, VB_BOT);
            return;
        case VSIG_FISHRACK:
            SET(VS_STEEL, 0x404040u); beam(c, at(&f, -hw, 0.0f, 0.0f), at(&f, hw, 0.0f, 0.0f), 0.02f);
            for (int k = 0; k < 5; ++k) { float x = -hw + w * ((float)k + 0.5f) / 5.0f; SET(VS_FOOD, tone(0xB8A070u, s + (uint32_t)k, 0.2f)); ibl(c, &f, x, -0.25f, 0.0f, 0.05f, 0.2f, 0.02f, s + (uint32_t)k); }
            return;
        case VSIG_CARGOBIKE:
            SET(VS_RUBBER, 0x151515u);
            cyl(c, at(&f, -0.7f, 0.33f, -0.02f), at(&f, -0.7f, 0.33f, 0.02f), 0.33f, sg, 0);
            cyl(c, at(&f, 0.6f, 0.25f, -0.02f), at(&f, 0.6f, 0.25f, 0.02f), 0.25f, sg, 0);
            SET(VS_STEEL, rgb); beam(c, at(&f, -0.7f, 0.33f, 0.0f), at(&f, -0.1f, 0.9f, 0.0f), 0.04f); beam(c, at(&f, -0.1f, 0.9f, 0.0f), at(&f, 0.6f, 0.25f, 0.0f), 0.04f);
            SET(VS_TIMBER, 0x7A5A3Au); ib(c, &f, 0.1f, 0.45f, -0.3f, 0.9f, 0.85f, 0.3f, 0);
            return;
        case VSIG_UMBRELLAS:
            for (int k = 0; k < 3; ++k) {
                float x = -0.3f + 0.3f * (float)k;
                SET(VS_CLOTH, tone(V_PALETTE[v->palette].c[2 + (k & 1)], s + (uint32_t)k, 0.15f));
                cone(c, at(&f, x, -0.55f, 0.0f), at(&f, x, -0.15f, 0.0f), 0.22f, sg6, 1);
            }
            return;
        case VSIG_MURAL: {
            SET(VS_PAPER, rgb);
            V3 a = at(&f, -hw, 0.0f, 0.0f), b = at(&f, hw, 0.0f, 0.0f), cc = at(&f, hw, h, 0.0f), dd = at(&f, -hw, h, 0.0f);
            quad(c, a, b, cc, dd);
            return;
        }
        case VSIG_SCREENS:
            for (int k = 0; k < 4; ++k) {
                float x = -hw + hw * (float)(k & 1), y = 0.36f * (float)(k >> 1);
                SET(VS_PROP, 0x1E1E1Eu); ib(c, &f, x + 0.02f, y, -hd, x + hw - 0.02f, y + 0.34f, hd, 0);
                set(c, VS_BULB, VCOL_RGB, lay, VPF_EMISSIVE, role, tone(0x60C8FFu, s + (uint32_t)k, 0.4f));
                V3 a = at(&f, x + 0.05f, y + 0.04f, hd + 0.005f), b = at(&f, x + hw - 0.05f, y + 0.04f, hd + 0.005f);
                quad(c, a, b, at(&f, x + hw - 0.05f, y + 0.3f, hd + 0.005f), at(&f, x + 0.05f, y + 0.3f, hd + 0.005f));
            }
            return;
        case VSIG_PENNANTS:
            SET(VS_STEEL, 0x303030u);
            ic(c, &f, -hw, 0.0f, 0.0f, 1.2f, 0.02f, 4, 0); ic(c, &f, hw, 0.0f, 0.0f, 1.2f, 0.02f, 4, 0);
            beam(c, at(&f, -hw, 1.2f, 0.0f), at(&f, hw, 1.2f, 0.0f), 0.01f);
            for (int k = 0; k < 8; ++k) {
                float x = -hw + w * ((float)k + 0.5f) / 8.0f;
                SET(VS_CLOTH, tone(V_PALETTE[v->palette].c[2 + (k & 1)], s + (uint32_t)k, 0.1f));
                float uv[3][2] = { { 0, 0 }, { 1, 0 }, { 0.5f, 1 } };
                V3 a = at(&f, x - 0.12f, 1.2f, 0.0f), b = at(&f, x + 0.12f, 1.2f, 0.0f), t = at(&f, x, 0.95f, 0.0f);
                tri(c, a, b, t, uv); tri(c, a, t, b, uv);
            }
            return;
        case VSIG_BIRDCAGE:
            SET(VS_CHROME, 0xC8B070u);
            beam(c, at(&f, 0, 0, 0), at(&f, 0, -0.12f, 0), 0.01f);
            cyl(c, at(&f, 0, -0.5f, 0), at(&f, 0, -0.22f, 0), 0.15f, sg6, 0);
            cone(c, at(&f, 0, -0.12f, 0), at(&f, 0, -0.22f, 0), 0.15f, sg6, 0);
            return;
        case VSIG_PLANTERS: {
            int np = w > 0.9f ? 3 : w > 0.6f ? 2 : 1;
            for (int k = 0; k < np; ++k) {
                float x = -hw + w * ((float)k + 0.5f) / (float)np;
                SET(VS_PROP, 0x8A5A3Au); ib(c, &f, x - 0.15f, 0.0f, -0.15f, x + 0.15f, 0.25f, 0.15f, VB_BOT);
                SET(VS_PLANT, tone(0x3A7A2Au, s + (uint32_t)k, 0.25f)); ibl(c, &f, x, 0.38f, 0.0f, 0.2f, 0.16f, 0.2f, s + (uint32_t)k);
            }
            return;
        }
        case VSIG_MASCOT:
            fl = VPF_SWAY;
            SET(VS_PROP, rgb); ibl(c, &f, 0.0f, 0.55f, 0.0f, 0.38f, 0.55f, 0.35f, s);
            SET(VS_PROP, 0xF0F0F0u); ibl(c, &f, 0.0f, 1.3f, 0.0f, 0.28f, 0.26f, 0.26f, s + 1u);
            return;
        default:
            return;
        }
    }
    switch (it->kind) {
    case VI_STOOL:
        SET(VS_CHROME, 0x9A9A9Au); ib(c, &f, -0.025f, 0.0f, -0.025f, 0.025f, h - 0.05f, 0.025f, VB_BOT | VB_TOP);
        SET(VS_PROP, tone(V_PALETTE[v->palette].c[v->cls == VC_RICH ? 1 : 2], s, 0.1f)); ic(c, &f, 0.0f, h - 0.05f, 0.0f, h, 0.17f, sg6, VB_BOT);
        SET(VS_CHROME, 0x9A9A9Au); ib(c, &f, -0.14f, 0.0f, -0.14f, 0.14f, 0.03f, 0.14f, VB_BOT);
        break;
    case VI_POT:
        SET(VS_STAINLESS, 0xB0B0B0u); ic(c, &f, 0.0f, 0.0f, 0.0f, h, hw, sg, VB_BOT);
        break;
    case VI_BOWLS: {
        int n = 2 + (int)(s % 3u);
        for (int k = 0; k < n; ++k) { SET(VS_PROP, tone(0xE8E2D6u, s + (uint32_t)k, 0.05f)); ic(c, &f, 0.0f, h * (float)k / (float)n, 0.0f, h * (float)(k + 1) / (float)n, hw * (1.0f - 0.08f * (float)k), sg6, VB_BOT); }
        break;
    }
    case VI_WOK:
        SET(VS_PROP, 0x202020u); ic(c, &f, 0.0f, 0.0f, 0.0f, h, hw, sg, VB_BOT);
        SET(VS_TIMBER, 0x6A4A2Au); beam(c, at(&f, hw, h * 0.8f, 0.0f), at(&f, hw + 0.25f, h, 0.0f), 0.03f);
        break;
    case VI_GRILL:
        SET(VS_STEEL, 0x2A2A2Au); ib(c, &f, -hw, 0.0f, -hd, hw, h * 0.8f, hd, VB_BOT);
        set(c, VS_BULB, VCOL_RGB, lay, VPF_EMISSIVE, role, 0xFF5010u); ib(c, &f, -hw + 0.03f, h * 0.8f, -hd + 0.03f, hw - 0.03f, h * 0.85f, hd - 0.03f, VB_BOT);
        SET(VS_FOOD, rgb); for (int k = 0; k < 4; ++k) beam(c, at(&f, -hw + 0.1f + 0.15f * (float)k, h * 0.9f, -hd + 0.05f), at(&f, -hw + 0.1f + 0.15f * (float)k, h * 0.9f, hd - 0.05f), 0.035f);
        break;
    case VI_STEAMER: {
        int n = 3;
        for (int k = 0; k < n; ++k) { SET(VS_TIMBER, tone(0xC8A86Au, s + (uint32_t)k, 0.06f)); ic(c, &f, 0.0f, h * (float)k / (float)n, 0.0f, h * (float)(k + 1) / (float)n - 0.01f, hw, sg, k < n - 1 ? VB_BOT | VB_TOP : VB_BOT); }
        break;
    }
    case VI_SCALE:
        SET(VS_STEEL, 0xC83A2Au); ib(c, &f, -hw, 0.0f, -hd, hw, h * 0.6f, hd, VB_BOT);
        SET(VS_STAINLESS, 0xC0C0C0u); ic(c, &f, 0.0f, h * 0.6f, 0.0f, h, hw * 0.9f, sg6, VB_BOT);
        break;
    case VI_CRATE: case VI_PARTSBIN: case VI_LUNCHBOXES:
        SET(it->kind == VI_CRATE ? VS_TIMBER : VS_PROP, it->kind == VI_CRATE ? tone(0x9A7A4Au, s, 0.15f) : tone(rgb, s, 0.15f));
        if (it->kind == VI_LUNCHBOXES) {
            for (int k = 0; k < 3; ++k) ib(c, &f, -hw, h * (float)k / 3.0f, -hd, hw, h * (float)(k + 1) / 3.0f - 0.005f, hd, VB_BOT);
        } else {
            ib(c, &f, -hw, 0.0f, -hd, hw, h, hd, VB_BOT | VB_TOP);
            SET(VS_FOOD, rgb); ib(c, &f, -hw + 0.03f, 0.0f, -hd + 0.03f, hw - 0.03f, h - 0.03f, hd - 0.03f, VB_BOT | VB_NX | VB_PX | VB_NZ | VB_PZ);
        }
        break;
    case VI_RADIO: case VI_CASHBOX:
        SET(VS_PROP, it->kind == VI_RADIO ? 0x3A3A3Au : 0x2A4A3Au); ib(c, &f, -hw, 0.0f, -hd, hw, h, hd, VB_BOT);
        break;
    case VI_BOTTLES: case VI_JARS: case VI_CUPS: case VI_CERAMICS: {
        int n = it->kind == VI_BOTTLES ? 4 : 3;
        for (int k = 0; k < n; ++k) {
            float x = -hw + w * ((float)k + 0.5f) / (float)n, hk = h * (it->kind == VI_CERAMICS ? 0.6f + 0.4f * hash_float(s + (uint32_t)k) : 1.0f);
            float r = it->kind == VI_BOTTLES ? 0.035f : minf(0.5f * w / (float)n, hd) * 0.9f;
            SET(it->kind == VI_BOTTLES || it->kind == VI_JARS ? VS_GLASS : VS_PROP, tone(it->kind == VI_CUPS ? 0xF0ECE4u : rgb, s + (uint32_t)k, 0.3f));
            ic(c, &f, x, 0.0f, 0.0f, hk, r, it->kind == VI_BOTTLES ? 4 : sg6, VB_BOT);
        }
        break;
    }
    case VI_FRUITBOX: case VI_ICETRAY:
        SET(it->kind == VI_ICETRAY ? VS_STAINLESS : VS_TIMBER, it->kind == VI_ICETRAY ? 0xC0C8CCu : 0xA88A5Au);
        ib(c, &f, -hw, 0.0f, -hd, hw, h * 0.6f, hd, VB_BOT);
        for (int k = 0; k < 4; ++k) {
            float x = -hw * 0.6f + hw * 0.4f * (float)k;
            SET(VS_FOOD, tone(V_TRADES[v->trade].goods[(s + (uint32_t)k) % 3u], s + (uint32_t)k, 0.15f));
            ibl(c, &f, x, h * 0.65f, 0.0f, 0.12f, 0.06f, hd * 0.7f, s + (uint32_t)k);
        }
        break;
    case VI_BUCKET: case VI_PLANTPOT:
        SET(VS_PROP, it->kind == VI_BUCKET ? 0x5A6A7Au : 0x9A5A3Au); ic(c, &f, 0.0f, 0.0f, 0.0f, h * 0.6f, hw, sg6, VB_BOT);
        SET(it->kind == VI_PLANTPOT ? VS_PLANT : VS_FOOD, it->kind == VI_PLANTPOT ? tone(0x3A7A2Au, s, 0.25f) : tone(rgb, s, 0.2f));
        ibl(c, &f, 0.0f, h * 0.8f, 0.0f, hw * 1.1f, h * 0.3f, hd * 1.1f, s);
        break;
    case VI_LOAVES:
        for (int k = 0; k < 3; ++k) { SET(VS_FOOD, tone(0xC88A4Au, s + (uint32_t)k, 0.12f)); ibl(c, &f, -hw + w * ((float)k + 0.5f) / 3.0f, 0.5f * h, 0.0f, w / 7.0f, 0.5f * h, hd, s + (uint32_t)k); }
        break;
    case VI_ROASTER:
        SET(VS_STEEL, 0x202020u); ib(c, &f, -hw, 0.0f, -hd, hw, h * 0.5f, hd, VB_BOT);
        cyl(c, at(&f, -hw * 0.9f, h * 0.75f, 0.0f), at(&f, hw * 0.9f, h * 0.75f, 0.0f), h * 0.25f, sg, 0);
        set(c, VS_BULB, VCOL_RGB, lay, VPF_EMISSIVE, role, 0xFF4A10u); ib(c, &f, -hw + 0.05f, h * 0.5f, -hd + 0.05f, hw - 0.05f, h * 0.52f, hd - 0.05f, VB_BOT);
        break;
    case VI_BLOCK:
        SET(VS_TIMBER, 0x8A6A4Au); ib(c, &f, -hw, 0.0f, -hd, hw, h, hd, VB_BOT);
        SET(VS_FOOD, tone(0xA82A2Au, s, 0.15f)); ibl(c, &f, 0.0f, h + 0.04f, 0.0f, hw * 0.5f, 0.05f, hd * 0.5f, s);
        break;
    case VI_BOOKSTACK:
        for (int k = 0; k < 4; ++k) { SET(VS_PROP, tone(V_TRADES[v->trade].goods[(s + (uint32_t)k) % 3u], s + (uint32_t)k, 0.2f)); ib(c, &f, -hw + 0.01f * (float)(k & 1), h * (float)k / 4.0f, -hd, hw - 0.015f * (float)(k & 2), h * (float)(k + 1) / 4.0f - 0.003f, hd, VB_BOT); }
        break;
    case VI_KETTLE:
        SET(VS_STAINLESS, 0xB8B8B8u); ic(c, &f, 0.0f, 0.0f, 0.0f, h * 0.8f, hw, sg6, VB_BOT);
        beam(c, at(&f, hw * 0.8f, h * 0.4f, 0.0f), at(&f, hw * 1.5f, h * 0.75f, 0.0f), 0.025f);
        break;
    case VI_ESPRESSO:
        SET(VS_STAINLESS, 0xC8C8C8u); ib(c, &f, -hw, 0.0f, -hd, hw, h, hd, VB_BOT);
        SET(VS_PROP, 0x202020u); for (int k = -1; k <= 1; k += 2) ic(c, &f, hw * 0.45f * (float)k, h * 0.35f, hd + 0.04f, h * 0.55f, 0.04f, 4, 0);
        break;
    case VI_TANK:
        SET(VS_GLASS, 0x6AA8C8u); ib(c, &f, -hw, 0.0f, -hd, hw, h, hd, VB_BOT);
        SET(VS_FOOD, tone(rgb, s, 0.2f)); ibl(c, &f, 0.0f, 0.5f * h, 0.0f, hw * 0.3f, h * 0.12f, hd * 0.25f, s);
        break;
    case VI_SCREEN:
        SET(VS_PROP, 0x3A3A36u); ib(c, &f, -hw, 0.0f, -hd, hw, h, hd, VB_BOT);
        set(c, VS_BULB, VCOL_RGB, lay, VPF_EMISSIVE, role, tone(0x70E0A0u, s, 0.3f));
        quad(c, at(&f, -hw + 0.04f, 0.05f, hd + 0.004f), at(&f, hw - 0.04f, 0.05f, hd + 0.004f), at(&f, hw - 0.04f, h - 0.05f, hd + 0.004f), at(&f, -hw + 0.04f, h - 0.05f, hd + 0.004f));
        break;
    case VI_HEAP:
        SET(VS_FOOD, rgb); blob(c, at(&f, -hw, 0.0f, -hd), at(&f, hw, h, hd), s);
        break;
    case VI_GASBOTTLE: case VI_DGAS:
        SET(VS_STEEL, rgb); ic(c, &f, 0.0f, 0.0f, 0.0f, h * 0.88f, 0.15f, sg6, VB_BOT);
        SET(VS_STEEL, 0x8A8A8Au); ic(c, &f, 0.0f, h * 0.88f, 0.0f, h, 0.04f, 4, VB_BOT);
        break;
    /* hanging: from its point down by h */
    case VI_LANTERN:
        SET(VS_STEEL, 0x202020u); beam(c, at(&f, 0, 0, 0), at(&f, 0, -0.08f, 0), 0.01f);
        set(c, VS_BULB, VCOL_RGB, lay, VPF_EMISSIVE | VPF_SWAY, role, rgb); ic(c, &f, 0.0f, -h, 0.0f, -0.08f, 0.13f, sg6, 0);
        break;
    case VI_BUNCH: case VI_DUCK: case VI_JOINT:
        SET(VS_STEEL, 0x303030u); beam(c, at(&f, 0, 0, 0), at(&f, 0, -0.15f, 0), 0.01f);
        SET(VS_FOOD, it->kind == VI_DUCK ? tone(0xA8501Eu, s, 0.1f) : tone(rgb, s, 0.2f));
        ibl(c, &f, 0.0f, -0.15f - 0.5f * (h - 0.15f), 0.0f, it->kind == VI_BUNCH ? 0.07f : 0.1f, 0.5f * (h - 0.15f), 0.07f, s);
        break;
    case VI_CABLE:
        SET(VS_RUBBER, 0x101010u);
        beam(c, at(&f, -0.1f, 0, 0), at(&f, 0.0f, -h, 0), 0.015f); beam(c, at(&f, 0.0f, -h, 0), at(&f, 0.12f, 0, 0), 0.015f);
        break;
    case VI_GARMENT:
        SET(VS_STEEL, 0x505050u); beam(c, at(&f, -0.18f, -0.08f, 0), at(&f, 0.18f, -0.08f, 0), 0.012f);
        SET(VS_CLOTH, tone(rgb, s, 0.25f));
        quad(c, at(&f, -0.2f, -0.08f, 0.0f), at(&f, 0.2f, -0.08f, 0.0f), at(&f, 0.2f, -h, 0.0f), at(&f, -0.2f, -h, 0.0f));
        quad(c, at(&f, -0.2f, -0.08f, 0.0f), at(&f, -0.2f, -h, 0.0f), at(&f, 0.2f, -h, 0.0f), at(&f, 0.2f, -0.08f, 0.0f));
        break;
    case VI_BUNTING: {
        /* triangles along the rail: p is its start, (fx, fz) along it, s[0] its length */
        V3 a = p3(it->p[0], it->p[1], it->p[2]), dir = p3(it->fx, 0, it->fz);
        int n = (int)(w / 0.3f);
        SET(VS_STEEL, 0x303030u); beam(c, a, add3(a, mul3(dir, w)), 0.008f);
        float uv[3][2] = { { 0, 0 }, { 1, 0 }, { 0.5f, 1 } };
        for (int k = 0; k < n; ++k) {
            V3 p0 = add3(a, mul3(dir, 0.3f * (float)k + 0.03f)), p1 = add3(a, mul3(dir, 0.3f * (float)k + 0.27f)), t = add3(add3(a, mul3(dir, 0.3f * (float)k + 0.15f)), p3(0, -h, 0));
            SET(VS_CLOTH, tone(V_PALETTE[v->palette].c[(k & 1) ? 2 : 3], s + (uint32_t)k, 0.1f));
            tri(c, p0, p1, t, uv); tri(c, p0, t, p1, uv);
        }
        break;
    }
    case VI_TOOLS:
        SET(VS_STEEL, 0x7A7A7Au);
        for (int k = 0; k < 3; ++k) beam(c, at(&f, -0.08f + 0.08f * (float)k, 0, 0), at(&f, -0.08f + 0.08f * (float)k, -h * (0.7f + 0.3f * hash_float(s + (uint32_t)k)), 0), 0.025f);
        break;
    case VI_UMBRELLA:
        SET(VS_STEEL, 0x303030u); beam(c, at(&f, 0, 0, 0), at(&f, 0, -0.2f, 0), 0.012f);
        SET(VS_CLOTH, tone(rgb, s, 0.3f)); cone(c, at(&f, 0, -h, 0), at(&f, 0, -0.2f, 0), 0.25f, sg6, 1);
        break;
    /* decorations */
    case VI_POSTER: case VI_STICKERS:
        SET(VS_PAPER, tone(rgb, s, 0.3f));
        quad(c, at(&f, -hw, 0.0f, hd), at(&f, hw, 0.0f, hd), at(&f, hw, h, hd), at(&f, -hw, h, hd));
        break;
    case VI_SPEAKER: case VI_CAMERA: case VI_EXTRACTOR: case VI_ACUNIT: case VI_COOLER:
        SET(VS_PROP, it->kind == VI_COOLER ? tone(rgb, s, 0.2f) : it->kind == VI_ACUNIT ? 0xC8C8C0u : 0x2A2A2Au);
        ib(c, &f, -hw, 0.0f, -hd, hw, h, hd, it->kind == VI_COOLER ? VB_BOT : 0);
        if (it->kind == VI_EXTRACTOR || it->kind == VI_ACUNIT) { SET(VS_PROP, 0x101010u); quad(c, at(&f, -hw * 0.7f, h * 0.15f, hd + 0.005f), at(&f, hw * 0.7f, h * 0.15f, hd + 0.005f), at(&f, hw * 0.7f, h * 0.85f, hd + 0.005f), at(&f, -hw * 0.7f, h * 0.85f, hd + 0.005f)); }
        break;
    case VI_PIPE:
        SET(VS_STEEL, 0x6A6A60u); ic(c, &f, 0.0f, 0.0f, 0.0f, h, hw, sg6, 0);
        break;
    case VI_DCRATE: case VI_STACKCRATES: {
        int n = it->kind == VI_STACKCRATES ? 3 : 1;
        for (int k = 0; k < n; ++k) { SET(VS_TIMBER, tone(0x9A7A4Au, s + (uint32_t)k, 0.2f)); ib(c, &f, -hw + 0.02f * (float)(k & 1), h * (float)k / (float)n, -hd, hw - 0.02f * (float)(k & 1), h * (float)(k + 1) / (float)n - 0.005f, hd, VB_BOT); }
        break;
    }
    case VI_BARREL: case VI_BIN: case VI_WATERTANK:
        SET(VS_STEEL, it->kind == VI_WATERTANK ? 0x2A4A8Au : tone(rgb, s, 0.25f)); ic(c, &f, 0.0f, 0.0f, 0.0f, h, hw, sg, VB_BOT);
        break;
    case VI_BAG: case VI_TRASHBAGS: {
        int n = it->kind == VI_TRASHBAGS ? 3 : 1;
        for (int k = 0; k < n; ++k) { SET(VS_PROP, it->kind == VI_TRASHBAGS ? 0x141414u : tone(0x8A7A5Au, s, 0.2f)); ibl(c, &f, -hw * 0.5f + hw * 0.5f * (float)k, 0.5f * h * (n > 1 ? 0.8f : 1.0f), 0.0f, n > 1 ? 0.22f : hw, 0.5f * h * (n > 1 ? 0.8f : 1.0f), hd, s + (uint32_t)k); }
        break;
    }
    case VI_BENCH:
        SET(VS_TIMBER, 0x8A6A4Au); ib(c, &f, -hw, h - 0.05f, -hd, hw, h, hd, 0);
        SET(VS_STEEL, 0x303030u); ib(c, &f, -hw + 0.1f, 0.0f, -hd + 0.05f, -hw + 0.15f, h - 0.05f, hd - 0.05f, VB_BOT | VB_TOP); ib(c, &f, hw - 0.15f, 0.0f, -hd + 0.05f, hw - 0.1f, h - 0.05f, hd - 0.05f, VB_BOT | VB_TOP);
        break;
    case VI_ROOFSTRING: case VI_LANTERNS:
        SET(VS_STEEL, 0x303030u);
        ic(c, &f, -hw, 0.0f, 0.0f, 0.9f, 0.02f, 4, 0); ic(c, &f, hw, 0.0f, 0.0f, 0.9f, 0.02f, 4, 0);
        beam(c, at(&f, -hw, 0.9f, 0.0f), at(&f, hw, 0.9f, 0.0f), 0.01f);
        for (int k = 0; k < 5; ++k) {
            float x = -hw + w * ((float)k + 0.5f) / 5.0f;
            set(c, VS_BULB, VCOL_RGB, lay, VPF_EMISSIVE, role, it->kind == VI_LANTERNS ? (k & 1 ? 0xC8281Eu : 0xE8B030u) : 0xFFE0A0u);
            if (it->kind == VI_LANTERNS) ic(c, &f, x, 0.62f, 0.0f, 0.88f, 0.1f, 5, 0);
            else ib(c, &f, x - 0.035f, 0.82f, -0.035f, x + 0.035f, 0.89f, 0.035f, 0);
        }
        break;
    case VI_ANTENNA:
        SET(VS_STEEL, 0x8A8A8Au); beam(c, at(&f, 0, 0, 0), at(&f, 0, h, 0), 0.03f);
        for (int k = 0; k < 3; ++k) beam(c, at(&f, -0.2f, h * (0.5f + 0.15f * (float)k), 0), at(&f, 0.2f, h * (0.5f + 0.15f * (float)k), 0), 0.015f);
        break;
    case VI_DISH:
        SET(VS_STEEL, 0x8A8A8Au); beam(c, at(&f, 0, 0, 0), at(&f, 0, 0.3f, 0), 0.03f);
        SET(VS_PROP, 0xD8D8D0u); cone(c, at(&f, 0, 0.3f, 0.12f), at(&f, 0, 0.42f, 0.0f), 0.28f, sg6, 1);
        break;
    case VI_VENTFAN:
        SET(VS_STEEL, 0x7A7A72u); ib(c, &f, -hw, 0.0f, -hd, hw, 0.12f, hd, VB_BOT); ic(c, &f, 0.0f, 0.12f, 0.0f, h, hw * 0.6f, sg6, 0);
        break;
    case VI_MENUBOARD: {
        SET(VS_SIGN, 0x1A1A1Au);
        V3 t0 = at(&f, -hw, h, 0.0f), t1 = at(&f, hw, h, 0.0f);
        quad(c, at(&f, -hw, 0.0f, hd), at(&f, hw, 0.0f, hd), t1, t0);
        quad(c, at(&f, hw, 0.0f, -hd), at(&f, -hw, 0.0f, -hd), t0, t1);
        break;
    }
    case VI_PLANTER:
        SET(VS_PROP, 0x6A6A60u); ib(c, &f, -hw, 0.0f, -hd, hw, h * 0.5f, hd, VB_BOT);
        SET(VS_PLANT, tone(0x3A7A2Au, s, 0.25f)); ibl(c, &f, 0.0f, h * 0.72f, 0.0f, hw, h * 0.25f, hd, s);
        break;
    case VI_CHAIR:
        SET(VS_PROP, tone(rgb, s, 0.2f));
        ib(c, &f, -hw, 0.42f, -hd, hw, 0.46f, hd, 0); ib(c, &f, -hw, 0.46f, -hd, hw, h, -hd + 0.04f, 0);
        SET(VS_STEEL, 0x404040u); ib(c, &f, -hw + 0.02f, 0.0f, -hd + 0.02f, -hw + 0.05f, 0.42f, hd - 0.02f, VB_BOT | VB_TOP); ib(c, &f, hw - 0.05f, 0.0f, -hd + 0.02f, hw - 0.02f, 0.42f, hd - 0.02f, VB_BOT | VB_TOP);
        break;
    default:
        SET(VS_PROP, rgb); ib(c, &f, -hw, 0.0f, -hd, hw, h, hd, VB_BOT);
        break;
    }
#undef SET
}

/* ------------------------------------------------------- signs and neon --- */

static uint32_t neon_word(int color, int motif, int flicker, int kind) {
    return (uint32_t)(color & 7) | (uint32_t)(motif & 255) << 8 | (uint32_t)(flicker & 3) << 16 | (uint32_t)kind << 24;
}

/* A board w x h centred at c facing n, depth t, with a neon face on its front (both, if blade). */
static void board(MCtx *c, const Vendor *v, const float cc[3], const float n[3], float w, float h, float t, int neon, uint32_t word, int both) {
    V3 o = qv(cc), ez = norm3(p3(n[0], 0.0f, n[2])), ey = p3(0, 1, 0), ex = cross3(ey, ez);
    set(c, VS_SIGN, VCOL_TRIM, VL_SHELL, 0, VR_SIGN, hash_combine(v->site.seed, (uint32_t)(cc[0] * 100.0f)) & 0xFFFFFFu);
    obox(c, o, ex, ey, ez, 0.5f * w, 0.5f * h, 0.5f * t, 0);
    set(c, VS_NEON, VCOL_RGB, VL_SHELL, neon ? VPF_EMISSIVE : 0, VR_NEON, word | (neon ? 0u : 1u << 30));   /* bit 30: painted, not lit */
    for (int s = 0; s < 1 + both; ++s) {
        V3 z = mul3(ez, s ? -(0.5f * t + 0.004f) : 0.5f * t + 0.004f), x = mul3(ex, s ? -1.0f : 1.0f);
        V3 a = add3(o, add3(z, add3(mul3(x, -0.47f * w), mul3(ey, -0.45f * h)))), b = add3(o, add3(z, add3(mul3(x, 0.47f * w), mul3(ey, -0.45f * h))));
        V3 cc2 = add3(o, add3(z, add3(mul3(x, 0.47f * w), mul3(ey, 0.45f * h)))), d = add3(o, add3(z, add3(mul3(x, -0.47f * w), mul3(ey, 0.45f * h))));
        quad(c, a, b, cc2, d);
    }
}

static void emit_signs(MCtx *c, const Vendor *v) {
    for (int i = 0; i < v->nsign; ++i) {
        const VSign *s = &v->sign[i];
        uint32_t word = neon_word(s->color, 255, s->flicker, 0);
        switch (s->kind) {
        case VSG_VALANCE:   /* painted on the valance */
            break;
        case VSG_BLADE: {
            float n[3] = { 1.0f, 0.0f, 0.0f };
            board(c, v, s->c, n, s->w, s->h, 0.06f, s->neon, word, 1);
            /* its bracket back to the post */
            set(c, VS_STEEL, VCOL_TRIM, VL_SHELL, 0, VR_SIGN, 0);
            beam(c, p3(s->c[0], s->c[1] + 0.5f * s->h + 0.03f, s->c[2] - 0.5f * s->w - 0.08f), p3(s->c[0], s->c[1] + 0.5f * s->h + 0.03f, s->c[2] + 0.5f * s->w), 0.03f);
            break;
        }
        case VSG_ROOF: {
            board(c, v, s->c, s->n, s->w, s->h, 0.08f, s->neon, word, 0);
            set(c, VS_STEEL, VCOL_TRIM, VL_SHELL, 0, VR_SIGN, 0);
            float y0 = s->c[1] - 0.5f * s->h - 0.12f;
            for (int k = -1; k <= 1; k += 2) {
                float x = s->c[0] + (float)k * 0.35f * s->w;
                beam(c, p3(x, y0, s->c[2] - 0.06f), p3(x, s->c[1] - 0.5f * s->h, s->c[2] - 0.06f), 0.04f);
            }
            break;
        }
        default:
            board(c, v, s->c, s->n, s->w, s->h, 0.05f, s->neon, word, 0);
            break;
        }
    }
    for (int i = 0; i < v->nneon; ++i) {
        const VNeon *N = &v->neon[i];
        V3 o = qv(N->c), ez = norm3(p3(N->n[0], 0, N->n[2])), ey = p3(0, 1, 0), ex = cross3(ey, ez);
        float hs = 0.5f * N->size;
        /* a dark frame the tubes are bent on, then the tubes */
        set(c, VS_SIGN, VCOL_TRIM, VL_SHELL, 0, VR_NEON, 0);
        obox(c, add3(o, mul3(ez, -0.03f)), ex, ey, ez, hs, hs, 0.02f, 0);
        set(c, VS_NEON, VCOL_RGB, VL_SHELL, VPF_EMISSIVE, VR_NEON, neon_word(N->color, N->motif, N->flicker, 1));
        V3 z = mul3(ez, -0.006f);
        quad(c, add3(o, add3(z, add3(mul3(ex, -hs), mul3(ey, -hs)))), add3(o, add3(z, add3(mul3(ex, hs), mul3(ey, -hs)))),
             add3(o, add3(z, add3(mul3(ex, hs), mul3(ey, hs)))), add3(o, add3(z, add3(mul3(ex, -hs), mul3(ey, hs)))));
        /* legs down to whatever it stands on, if it stands on a roof */
        if (fabsf(N->n[0]) < 0.5f && N->c[1] - hs > 1.0f) {
            set(c, VS_STEEL, VCOL_TRIM, VL_SHELL, 0, VR_NEON, 0);
            for (int k = -1; k <= 1; k += 2) beam(c, add3(o, add3(mul3(ex, (float)k * 0.6f * hs), add3(mul3(ey, -hs), mul3(ez, -0.05f)))),
                                                   add3(o, add3(mul3(ex, (float)k * 0.6f * hs), add3(mul3(ey, -hs - 0.12f), mul3(ez, -0.05f)))), 0.03f);
        }
    }
}

static int build(const Vendor *v, int layer, VendorMesh *m) {
    MCtx c;
    memset(&c, 0, sizeof(c));
    c.m = m; c.layer = layer;
    for (int i = 0; i < v->npart; ++i) emit_part(&c, v, &v->part[i]);
    for (int i = 0; i < v->nitem; ++i) emit_item(&c, v, &v->item[i]);
    c.lay = VL_SHELL;
    if (wanted(&c)) emit_signs(&c, v);
    return c.count;
}

int vendor_mesh(const Vendor *v, int layer, VendorMesh *m) { return build(v, layer, m); }
int vendor_tris(const Vendor *v, int layer) { return build(v, layer, NULL); }
void vendor_part_aabb(const VPart *p, float mn[3], float mx[3]) { v_part_aabb(p, mn, mx); }
void vendor_mesh_free(VendorMesh *m) { free(m->v); m->v = NULL; m->count = m->cap = 0; }
