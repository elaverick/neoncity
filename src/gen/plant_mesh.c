#include "plant_internal.h"

/* ===========================================================================
 * Organ geometry (stage 6)
 * ========================================================================= */

static float profile(const Shape *s, float t) {
    if (t <= 0.0f) return s->w[0];
    for (int i = 1; i < s->n; ++i)
        if (t <= s->t[i]) {
            float f = (t - s->t[i - 1]) / (s->t[i] - s->t[i - 1]);
            f = f * f * (3.0f - 2.0f * f) * 0.5f + f * 0.5f;
            return lerpf(s->w[i - 1], s->w[i], f);
        }
    return 0.0f;
}

typedef struct PV { vec3 p, n; float u, v; } PV;

static void push_vert(Ctx *c, const PV *a, int part, int kind, int tint, uint32_t organ) {
    PlantMesh *m = c->out;
    if (m->count == m->cap) {
        m->cap = m->cap ? m->cap * 2 : 8192;
        m->v = (PlantVertex *)realloc(m->v, sizeof(PlantVertex) * m->cap);
        if (!m->v) oom();
    }
    PlantVertex *o = &m->v[m->count++];
    o->p[0] = a->p.x; o->p[1] = a->p.y; o->p[2] = a->p.z;
    o->n[0] = a->n.x; o->n[1] = a->n.y; o->n[2] = a->n.z;
    o->uv[0] = a->u; o->uv[1] = a->v;
    o->part = (uint8_t)part; o->kind = (uint8_t)kind; o->tint = (uint8_t)tint; o->ao = 255;
    o->organ = (uint16_t)(organ & 0x7FFFu); o->pad = 0;
}

/* A triangle wound to face the side its vertex normals face. */
static void tri(Ctx *c, const PV *a, const PV *b, const PV *d, int part, int kind, int tint, uint32_t organ) {
    vec3 cr = v3_cross(v3_sub(b->p, a->p), v3_sub(d->p, a->p));
    if (v3_dot(cr, cr) < 1e-14f) return;
    vec3 nn = v3_add(v3_add(a->n, b->n), d->n);
    if (v3_dot(cr, nn) < 0.0f) { const PV *t = b; b = d; d = t; }
    push_vert(c, a, part, kind, tint, organ); push_vert(c, b, part, kind, tint, organ); push_vert(c, d, part, kind, tint, organ);
}

static int tint_of(const Ctx *c, uint32_t path) {
    float v = (rnd(c, path, CH_TINT) * 2.0f - 1.0f) * clampf(c->P.tintVariation / 0.08f, 0.0f, 1.0f);
    return (int)clampf(128.0f + v * 127.0f, 0.0f, 255.0f);
}

typedef struct Blade {
    const Shape *s;
    vec3  o, D, S, N;
    float len, wid, droop, twist, flex, phase, floorY;
} Blade;

/* A point on the blade surface at (t along, u across), after camber, edge
 * ripple, twist and the world-down sag of section 2.4. */
static vec3 blade_point(const Ctx *c, const Blade *b, float t, float u) {
    float ang = b->twist * t, ca = cosf(ang), sa = sinf(ang);
    vec3 S = v3_add(v3_scale(b->S, ca), v3_scale(b->N, sa)), N = v3_sub(v3_scale(b->N, ca), v3_scale(b->S, sa));
    float w = profile(b->s, t) * 0.5f * b->wid;
    float lift = b->s->camber * b->wid * 0.5f * u * u + b->s->wave * b->wid * sinf(b->s->waveFreq * PI_F * t + b->phase) * powf(fabsf(u), 1.5f);
    vec3 p = madd(madd(madd(b->o, b->D, t * b->len), S, u * w), N, lift);
    float sag = (b->droop * b->len + c->G * b->len * b->flex) * t * t + c->G * b->len * b->s->edgeFlex * u * u * sinf(PI_F * t);
    p.y -= minf(sag, 0.65f * b->len * t * t + 0.02f * b->len);
    if (p.y < b->floorY) p.y = b->floorY;
    return p;
}

static void emit_blade(Ctx *c, vec3 base, vec3 D, vec3 Uhint, int shape, float len, float wid, float droop, float twist, uint32_t path) {
    if (!(len > 0.003f) || !(wid > 0.0005f)) return;
    const Shape *s = &SHAPES[shape];
    Blade b;
    b.s = s; b.o = base; b.D = D; b.len = len; b.wid = wid; b.droop = droop; b.twist = twist;
    b.flex = s->flex * clampf(0.6f + wid / len, 0.6f, 1.4f);
    b.phase = rnd(c, path, CH_PHASE) * 6.28f;
    b.floorY = -c->P.floorDepth + 0.02f;
    /* the blade faces up, turned as the grammar's twist says */
    vec3 N = v3_sub(UP, v3_scale(D, v3_dot(UP, D)));
    if (v3_len(N) < 0.15f) N = v3_sub(Uhint, v3_scale(D, v3_dot(Uhint, D)));
    N = v3_norm(N);
    b.S = v3_norm(v3_cross(D, N));
    b.N = v3_cross(b.S, D);
    int na = s->along[c->q] - c->detail, nc = s->across[c->q];
    if (na < 1) na = 1;
    if (nc > 2 && c->detail > 0) nc = 2;
    /* uv: across, as a fraction of the widest half-width (the texture's scale
       across the blade); along, t plus twice the blade's length / width in
       quarters, constant over the blade (the shader's tiling along it) */
    float aspect = roundf(clampf(len / wid, 0.25f, 60.0f) * 4.0f);
    PV g[11][5];
    for (int j = 0; j <= na; ++j) {
        float t = (float)j / (float)na;
        for (int i = 0; i <= nc; ++i) {
            float u = -1.0f + 2.0f * (float)i / (float)nc;
            vec3 p = blade_point(c, &b, t, u);
            float et = 0.01f, eu = 0.02f;
            vec3 dt = v3_sub(blade_point(c, &b, minf(t + et, 1.0f), u), blade_point(c, &b, maxf(t - et, 0.0f), u));
            vec3 du = v3_sub(blade_point(c, &b, t, minf(u + eu, 1.0f)), blade_point(c, &b, t, maxf(u - eu, -1.0f)));
            vec3 n = v3_cross(du, dt);
            if (v3_len(n) < 1e-9f) n = b.N;
            n = v3_norm(n);
            if (v3_dot(n, b.N) < 0.0f) n = v3_scale(n, -1.0f);
            PV *q = &g[j][i];
            q->p = p; q->n = n; q->u = u * profile(s, t); q->v = t + 2.0f * aspect;
        }
    }
    int tint = tint_of(c, path);
    for (int j = 0; j < na; ++j)
        for (int i = 0; i < nc; ++i) {
            tri(c, &g[j][i], &g[j][i + 1], &g[j + 1][i + 1], PP_LEAF, shape, tint, path);
            tri(c, &g[j][i], &g[j + 1][i + 1], &g[j + 1][i], PP_LEAF, shape, tint, path);
        }
    c->out->st.leaves++;
}

/* A petiole of two to four segments, curving down with gravity; returns the
 * blade's attachment point and direction (its terminal tangent). */
static vec3 petiole(Ctx *c, vec3 base, vec3 D, float len, float flex, float r, uint32_t path, int mat, vec3 *tangent) {
    static const int NSEG[PQ_COUNT] = { 2, 3, 4 };
    float drop = minf(c->G * len * flex, 0.5f * len);
    int n = NSEG[c->q], prev = -1;
    vec3 a = base;
    for (int k = 1; k <= n; ++k) {
        float t = (float)k / (float)n;
        vec3 b = madd(base, D, len * t);
        b.y -= drop * t * t;
        prev = add_seg(c, a, b, r, r * 0.8f, mat, prev, 3, 0, k == 1 ? r * 2.0f : 0.0f, path);
        a = b;
    }
    *tangent = v3_norm(v3(D.x * len, D.y * len - 2.0f * drop, D.z * len));
    return a;
}

/* A simple leaf: petiole, then blade, from the stem's surface. */
static void emit_leaf(Ctx *c, vec3 p, vec3 D, vec3 Uhint, float stemR, int shape, const float *a, uint32_t path) {
    float len = a[0], wid = a[1], pet = a[2], droop = a[3], twist = a[4];
    vec3 base = madd(p, D, stemR * 0.85f);
    if (pet > 0.012f) {
        vec3 T;
        float pr = clampf(0.03f * wid, 0.0016f, 0.006f);
        base = petiole(c, base, D, pet, shape == LS_FIDDLE ? 0.35f : 0.3f, pr, path, WM_GREEN, &T);
        D = T;
    }
    emit_blade(c, base, D, Uhint, shape, len, wid, droop, twist, path);
}

/* A pinnate frond: a curved rachis with leaflets in subtly offset pairs,
 * shorter toward the tip, hanging from it in an open V. */
static void emit_frond(Ctx *c, const Organ *o) {
    static const int RS[PQ_COUNT] = { 5, 7, 9 };
    float len = o->a[0], lmax = o->a[1], droop = o->a[3], open = o->a[4];
    int pairs = (int)(o->a[2] * c->leafletKeep + 0.5f), n = RS[c->q];
    if (pairs < 3) pairs = 3;
    vec3 D0 = o->F;
    vec3 hd = v3(D0.x, 0.0f, D0.z);
    float hl = v3_len(hd);
    hd = hl > 1e-4f ? v3_scale(hd, 1.0f / hl) : perp_of(UP);
    float e0 = asinf(clampf(D0.y, -1.0f, 1.0f));
    float bend = droop * 1.1f + c->G * 0.7f;
    float yawDrift = jit(c, o->path, CH_TWIST, 0.15f);
    vec3 pt[12], tg[12];
    float hs = c->H / plant_info(PS_KENTIA)->refHeight;
    for (int k = 0; k <= n; ++k) {
        float s = (float)k / (float)n, e = maxf(e0 - bend * powf(s, 1.4f), -80.0f * DEG);
        vec3 h = rot_axis(hd, UP, yawDrift * s);
        tg[k] = v3_norm(madd(v3_scale(h, cosf(e)), UP, sinf(e)));
    }
    pt[0] = o->p;
    for (int k = 1; k <= n; ++k) pt[k] = madd(madd(pt[k - 1], tg[k - 1], len / (float)n * 0.5f), tg[k], len / (float)n * 0.5f);
    for (int k = 0; k <= n; ++k) if (pt[k].y < -c->P.floorDepth + 0.03f) pt[k].y = -c->P.floorDepth + 0.03f;
    int prev = -1;
    float rb = 0.0075f * hs, rt = 0.0015f * hs;
    for (int k = 0; k < n; ++k)
        prev = add_seg(c, pt[k], pt[k + 1], lerpf(rb, rt, (float)k / (float)n), lerpf(rb, rt, (float)(k + 1) / (float)n), WM_GREEN, prev, 2, 0, k == 0 ? rb * 1.5f : 0.0f, o->path);
    for (int side = 0; side < 2; ++side)
        for (int i = 0; i < pairs; ++i) {
            float s = 0.16f + 0.82f * ((float)i + 0.5f + (side ? 0.35f : 0.0f)) / (float)pairs;
            if (s > 0.995f) continue;
            float fk = s * (float)n;
            int k = (int)fk;
            if (k >= n) k = n - 1;
            float f = fk - (float)k;
            vec3 P = v3_add(v3_scale(pt[k], 1.0f - f), v3_scale(pt[k + 1], f)), T = v3_norm(v3_add(v3_scale(tg[k], 1.0f - f), v3_scale(tg[k + 1], f)));
            vec3 Nf = v3_sub(UP, v3_scale(T, v3_dot(UP, T)));
            Nf = v3_len(Nf) > 1e-3f ? v3_norm(Nf) : o->U;
            vec3 S = v3_scale(v3_norm(v3_cross(T, Nf)), side ? 1.0f : -1.0f);
            uint32_t lp = child(o->path, TAG_L, i * 2 + side);
            float a = (62.0f - 26.0f * s) * DEG * open + jit(c, lp, CH_AZ, 4.0f * DEG);
            vec3 d = v3_norm(v3_add(v3_scale(T, cosf(a)), v3_scale(S, sinf(a))));
            d = v3_norm(madd(d, Nf, -(0.12f + 0.55f * s * droop) * open));
            float prof = (0.5f + 0.5f * sinf(PI_F * 0.5f * clampf((s - 0.16f) / 0.42f, 0.0f, 1.0f))) * (1.0f - 0.62f * smoothstepf(0.45f, 1.0f, s));
            float l = lmax * prof * (1.0f + jit(c, lp, CH_LEN, c->lv * 0.5f));
            emit_blade(c, madd(P, d, 0.004f), d, Nf, LS_KENTIA, l, l * 0.1f, 0.05f + 0.1f * s, jit(c, lp, CH_TWIST, 0.3f) + (side ? 0.25f : -0.25f), lp);
            c->out->st.leaflets++;
            c->out->st.leaves--;
        }
    c->out->st.organs++;
}

/* A palmate umbrella: a petiole to a hub, then elliptic leaflets on short
 * petiolules radiating from it, the umbrella tilted to the light. */
static void emit_umbrella(Ctx *c, const Organ *o) {
    int n = (int)o->a[0];
    float len = o->a[1], wid = o->a[2], droop = o->a[3], pet = o->a[4], open = o->a[5];
    if (c->leafletKeep < 1.0f) n = (int)maxf(5.0f, roundf((float)n * (0.5f + 0.5f * c->leafletKeep)));
    vec3 T, hub = petiole(c, madd(o->p, o->F, o->stemR * 0.85f), o->F, pet, 0.3f, clampf(0.035f * len, 0.0016f, 0.004f), o->path, WM_GREEN, &T);
    vec3 Nh = v3_norm(v3_add(v3_scale(T, 0.4f), v3_scale(UP, 0.6f)));
    vec3 A1 = v3_norm(v3_sub(T, v3_scale(Nh, v3_dot(T, Nh))));
    if (!finite3(A1) || v3_len(A1) < 0.5f) A1 = perp_of(Nh);
    vec3 A2 = v3_cross(Nh, A1);
    float el = lerpf(50.0f, -6.0f, open) * DEG;
    for (int i = 0; i < n; ++i) {
        uint32_t lp = child(o->path, TAG_L, i);
        float phi = 2.0f * PI_F * (float)i / (float)n + jit(c, lp, CH_AZ, 0.12f);
        vec3 radial = v3_add(v3_scale(A1, cosf(phi)), v3_scale(A2, sinf(phi)));
        float e = el + jit(c, lp, CH_PITCH, 8.0f * DEG);
        vec3 d = v3_norm(v3_add(v3_scale(radial, cosf(e)), v3_scale(Nh, sinf(e))));
        float l = len * (0.72f + 0.28f * cosf(phi)) * (1.0f + jit(c, lp, CH_LEN, c->lv));
        emit_blade(c, madd(hub, d, 0.1f * l), d, Nh, LS_UMBRELLA, l, wid * l / len, droop * 0.4f, jit(c, lp, CH_TWIST, 0.3f), lp);
        c->out->st.leaflets++;
        c->out->st.leaves--;
    }
    c->out->st.organs++;
}

/* A terminal cluster of small leaves round the tip. */
static void emit_cluster(Ctx *c, const Organ *o) {
    int n = (int)o->a[2];
    for (int i = 0; i < n; ++i) {
        uint32_t lp = child(o->path, TAG_L, i);
        float roll = (float)i * GOLDEN + jit(c, lp, CH_AZ, 0.4f), pitch = lerp_r(c, lp, CH_PITCH, 15.0f, 45.0f) * DEG;
        vec3 radial = v3_add(v3_scale(o->R, cosf(roll)), v3_scale(o->U, sinf(roll)));
        vec3 d = v3_norm(v3_add(v3_scale(o->F, cosf(pitch)), v3_scale(radial, sinf(pitch))));
        float len = o->a[0] * lerp_r(c, lp, CH_LEN, 0.7f, 1.0f);
        float a[5] = { len, len * o->a[1], 0.0f, 0.04f, jit(c, lp, CH_TWIST, 0.5f) };
        emit_leaf(c, o->p, d, o->U, o->stemR * 0.5f, o->shape, a, lp);
    }
}

/* An aerial root: a thin tapered curve hanging toward the soil, trimmed there. */
static void emit_root(Ctx *c, const Organ *o) {
    int n = 4, prev = -1;
    vec3 a = o->p, d = o->F;
    for (int k = 0; k < n; ++k) {
        d = v3_norm(madd(d, v3(0, -1, 0), o->a[1]));
        vec3 b = madd(a, d, o->a[0] / (float)n);
        if (b.y < 0.0f) break;
        prev = add_seg(c, a, b, o->a[2] * (1.0f - 0.2f * (float)k), o->a[2] * (1.0f - 0.2f * (float)(k + 1)), WM_ROOT, prev, 3, 0, 0.0f, o->path);
        a = b;
    }
}

void emit_organs(Ctx *c) {
    for (int i = 0; i < c->norg; ++i) {
        const Organ *o = &c->org[i];
        switch (o->type) {
        case O_LEAF:     emit_leaf(c, o->p, o->F, o->U, o->stemR, o->shape, o->a, o->path); break;
        case O_FROND:    emit_frond(c, o); break;
        case O_UMBRELLA: emit_umbrella(c, o); break;
        case O_CLUSTER:  emit_cluster(c, o); break;
        case O_ROOT:     emit_root(c, o); break;
        }
    }
}

/* ---- tapered tubes for every stem segment ---- */

int sides_for(const Ctx *c, float r) {
    static const int BIG[PQ_COUNT] = { 6, 8, 10 }, MID[PQ_COUNT] = { 4, 5, 6 };
    int s = r > 0.012f ? BIG[c->q] : r > 0.0045f ? MID[c->q] : 3;
    s -= c->detail;
    return s < 3 ? 3 : s;
}

static float flare(const Ctx *c, const Seg *s, float y) {
    if (!(s->flags & SF_FLARE)) return 1.0f;
    float f = 1.0f - clampf(y / (0.06f * c->H), 0.0f, 1.0f);
    return 1.0f + 0.4f * f * f;
}

static void ring(const Ctx *c, const Seg *s, vec3 p, vec3 T, vec3 ref, float r, int sides, float v, PV *out) {
    vec3 X = v3_sub(ref, v3_scale(T, v3_dot(ref, T)));
    X = v3_len(X) > 1e-5f ? v3_norm(X) : perp_of(T);
    vec3 Y = v3_cross(T, X);
    r *= flare(c, s, p.y);
    for (int j = 0; j <= sides; ++j) {
        float a = 2.0f * PI_F * (float)j / (float)sides;
        vec3 n = v3_add(v3_scale(X, cosf(a)), v3_scale(Y, sinf(a)));
        out[j].p = madd(p, n, r); out[j].n = n; out[j].u = (float)j / (float)sides; out[j].v = v;
    }
}

void emit_tubes(Ctx *c) {
    int n = c->nseg;
    int *next = (int *)malloc(sizeof(int) * (size_t)(n ? n : 1));
    vec3 *endRef = (vec3 *)malloc(sizeof(vec3) * (size_t)(n ? n : 1));
    float *endV = (float *)malloc(sizeof(float) * (size_t)(n ? n : 1));
    if (!next || !endRef || !endV) oom();
    for (int i = 0; i < n; ++i) next[i] = -1;
    for (int i = 0; i < n; ++i) if (c->seg[i].prev >= 0 && next[c->seg[i].prev] < 0) next[c->seg[i].prev] = i;
    PV r0[12], r1[12];
    for (int i = 0; i < n; ++i) {
        Seg *s = &c->seg[i];
        vec3 dir = v3_sub(s->b, s->a);
        float len = v3_len(dir);
        if (len < 1e-5f) { endRef[i] = s->prev >= 0 ? endRef[s->prev] : perp_of(UP); endV[i] = s->prev >= 0 ? endV[s->prev] : 0.0f; continue; }
        dir = v3_scale(dir, 1.0f / len);
        int cont = s->prev >= 0 && next[s->prev] == i;
        s->sides = (uint8_t)(cont ? c->seg[s->prev].sides : sides_for(c, s->r0));
        int sides = s->sides;
        vec3 a = s->a, T0 = dir, ref;
        float v0 = 0.0f;
        if (cont) {
            vec3 pd = v3_norm(v3_sub(c->seg[s->prev].b, c->seg[s->prev].a));
            T0 = v3_norm(v3_add(pd, dir));
            ref = endRef[s->prev];
            v0 = endV[s->prev];
        } else {
            ref = rot_axis(perp_of(dir), dir, hash_float(s->path) * 6.28f);
            float back = minf(s->attachR * 0.8f, 0.3f * len);
            if (s->flags & SF_SOIL) back = 0.04f;
            a = madd(a, dir, -back);
        }
        vec3 T1 = next[i] >= 0 ? v3_norm(v3_add(dir, v3_norm(v3_sub(c->seg[next[i]].b, c->seg[next[i]].a)))) : dir;
        if (!finite3(T0) || v3_len(T0) < 0.5f) T0 = dir;
        if (!finite3(T1) || v3_len(T1) < 0.5f) T1 = dir;
        float v1 = v0 + len;
        ring(c, s, a, T0, ref, s->r0, sides, v0, r0);
        vec3 ref1 = v3_sub(ref, v3_scale(T1, v3_dot(ref, T1)));
        ref1 = v3_len(ref1) > 1e-5f ? v3_norm(ref1) : perp_of(T1);
        ring(c, s, s->b, T1, ref1, s->r1, sides, v1, r1);
        endRef[i] = ref1; endV[i] = v1;
        for (int j = 0; j < sides; ++j) {
            tri(c, &r0[j], &r0[j + 1], &r1[j + 1], PP_WOOD, s->mat, 128, s->path);
            tri(c, &r0[j], &r1[j + 1], &r1[j], PP_WOOD, s->mat, 128, s->path);
        }
        if (next[i] < 0 && s->r1 > 0.0015f) {   /* close the tip */
            PV tip;
            tip.p = madd(s->b, dir, s->r1 * 0.6f); tip.n = dir; tip.u = 0.5f; tip.v = v1 + s->r1;
            for (int j = 0; j < sides; ++j) {
                PV e0 = r1[j], e1 = r1[j + 1];
                e0.n = v3_norm(v3_add(e0.n, dir)); e1.n = v3_norm(v3_add(e1.n, dir));
                tri(c, &e0, &e1, &tip, PP_WOOD, s->mat, 128, s->path);
            }
        }
    }
    c->out->st.segments = n;
    free(next); free(endRef); free(endV);
}
