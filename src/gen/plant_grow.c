#include "plant_internal.h"

/* ===========================================================================
 * Expansion (stage 2)
 * ========================================================================= */

static void produce(Ctx *c, const Mod *m, MList *o) {
    switch (c->sp) {
    case PS_KENTIA:     prod_kentia(c, m, o); break;
    case PS_DRACAENA:   prod_dracaena(c, m, o); break;
    case PS_LYRATA:     prod_lyrata(c, m, o); break;
    case PS_BENJAMINA:  prod_benjamina(c, m, o); break;
    case PS_SCHEFFLERA: prod_schefflera(c, m, o); break;
    default:            prod_zz(c, m, o); break;
    }
}

/* Parallel rewriting until nothing is left to rewrite, the pass limit is
 * reached (remaining buds are pruned) or the module budget is spent. */
int expand(Ctx *c) {
    for (c->passes = 0; c->passes < MAX_PASSES; ++c->passes) {
        int any = 0;
        c->nxt.n = 0;
        for (int i = 0; i < c->cur.n; ++i) {
            const Mod *m = &c->cur.m[i];
            if (m->s >= N_FIRST) { any = 1; if (c->nxt.n < MAX_MODS) produce(c, m, &c->nxt); }
            else *put(&c->nxt, m->s) = *m;
        }
        MList t = c->cur; c->cur = c->nxt; c->nxt = t;
        if (!any) break;
    }
    /* buds left after the last pass are pruned */
    int n = 0, depth = 0;
    for (int i = 0; i < c->cur.n; ++i) {
        if (c->cur.m[i].s >= N_FIRST) continue;
        if (c->cur.m[i].s == T_PUSH) depth++;
        if (c->cur.m[i].s == T_POP && --depth < 0) return 0;
        c->cur.m[n++] = c->cur.m[i];
    }
    c->cur.n = n;
    return depth == 0;   /* brackets must balance */
}

/* ===========================================================================
 * Turtle interpretation (stage 3), with the centre-line refinement
 * ========================================================================= */

int add_seg(Ctx *c, vec3 a, vec3 b, float r0, float r1, int mat, int prev, int depth, int flags, float attachR, uint32_t path) {
    if (c->nseg == c->capseg) {
        c->capseg = c->capseg ? c->capseg * 2 : 512;
        c->seg = (Seg *)realloc(c->seg, sizeof(Seg) * (size_t)c->capseg);
        if (!c->seg) oom();
    }
    Seg *s = &c->seg[c->nseg];
    s->a = a; s->b = b; s->r0 = r0; s->r1 = r1; s->attachR = attachR;
    s->prev = prev; s->mat = (uint8_t)mat; s->flags = (uint8_t)flags; s->depth = (uint8_t)(depth > 255 ? 255 : depth); s->sides = 0;
    s->path = path;
    return c->nseg++;
}

static Organ *add_organ(Ctx *c, int type, const Turtle *t, uint32_t path, int prio) {
    if (c->norg == c->caporg) {
        c->caporg = c->caporg ? c->caporg * 2 : 256;
        c->org = (Organ *)realloc(c->org, sizeof(Organ) * (size_t)c->caporg);
        if (!c->org) oom();
    }
    Organ *o = &c->org[c->norg++];
    memset(o, 0, sizeof(*o));
    o->type = (uint8_t)type; o->path = path; o->prio = (uint8_t)prio;
    o->p = t->p; o->F = t->F; o->R = t->R; o->U = t->U; o->stemR = t->r;
    return o;
}

/* Turns the turtle frame so forward becomes d (the smallest rotation). */
static void orient(Turtle *t, vec3 d) {
    vec3 ax = v3_cross(t->F, d);
    float s = v3_len(ax), co = v3_dot(t->F, d);
    if (s > 1e-6f) {
        ax = v3_scale(ax, 1.0f / s);
        float a = atan2f(s, co);
        t->R = v3_norm(rot_axis(t->R, ax, a));
        t->U = v3_norm(rot_axis(t->U, ax, a));
    }
    t->F = d;
    t->U = v3_norm(v3_sub(t->U, v3_scale(d, v3_dot(t->U, d))));
    t->R = v3_cross(t->F, t->U);
}

static float material_flex(int mat) {
    return mat == WM_PALM ? 0.0f : mat == WM_CANE ? 0.3f : mat == WM_ZZ ? 0.35f : 1.0f;
}

void interpret(Ctx *c) {
    Turtle st[MAX_STACK];
    int sp = 0;
    Turtle t;
    memset(&t, 0, sizeof(t));
    t.p = v3(0, 0, 0); t.F = UP; t.R = v3(1, 0, 0); t.U = v3(0, 0, 1);
    t.last = -1; t.fresh = 1;
    float H = c->H;
    for (int i = 0; i < c->cur.n; ++i) {
        const Mod *m = &c->cur.m[i];
        switch (m->s) {
        case T_F: {
            float l = m->a[0], r0 = m->a[1], r1 = m->a[2];
            if (!(l > 1e-5f)) break;
            vec3 d = t.F;
            /* gravity: a cumulative parabolic droop along the branch run (2.3) */
            float flex = clampf(m->a[3] * material_flex(m->k) * (1.0f - r0 / (0.03f * H)), 0.0f, 1.0f);
            if (c->G > 0.0f && flex > 0.0f) {
                float h = sqrtf(d.x * d.x + d.z * d.z), s0 = t.run, s1 = s0 + l;
                float drop = minf(0.45f * l, c->G * 0.82f * flex * h * (s1 * s1 - s0 * s0) / H);
                d = v3_norm(v3(d.x, d.y - drop / l, d.z));
            }
            if (m->a[5] > 0.0f) d = v3_norm(madd(d, UP, m->a[5] * l / maxf(0.1f * H, 0.05f)));
            /* coherent centre-line noise, fading to nothing at the axis's junction */
            if (m->a[4] > 0.0f) {
                vec3 nv = noise_vec(c, m->axis, t.p);
                nv = v3_sub(nv, v3_scale(d, v3_dot(nv, d)));
                float fade = smoothstepf(0.0f, 2.5f * l, t.run);
                d = v3_norm(madd(d, nv, m->a[4] * fade));
            }
            orient(&t, d);
            vec3 b = madd(t.p, d, l);
            int flags = (t.p.y < 0.01f && t.fresh) ? SF_SOIL : 0;
            if (t.depth <= 1 && t.p.y < 0.12f * H && (m->k == WM_PALM || m->k == WM_CANE || m->k == WM_FICUS || m->k == WM_UMBRELLA)) flags |= SF_FLARE;
            int s = add_seg(c, t.p, b, r0, r1, m->k, t.fresh ? -1 : t.last, t.depth, flags, t.fresh ? t.attachR : 0.0f, m->path);
            t.p = b; t.r = r1; t.run += l; t.last = s; t.fresh = 0;
            break;
        }
        case T_f:     t.p = madd(t.p, t.F, m->a[0]); t.fresh = 1; t.last = -1; break;
        case T_YAW:   { float a = m->a[0]; vec3 F = t.F; t.F = v3_norm(madd(v3_scale(F, cosf(a)), t.R, sinf(a))); t.R = v3_norm(madd(v3_scale(t.R, cosf(a)), F, -sinf(a))); break; }
        case T_PITCH: { float a = m->a[0]; vec3 F = t.F; t.F = v3_norm(madd(v3_scale(F, cosf(a)), t.U, sinf(a))); t.U = v3_norm(madd(v3_scale(t.U, cosf(a)), F, -sinf(a))); break; }
        case T_ROLL:  { float a = m->a[0]; vec3 R = t.R; t.R = v3_norm(madd(v3_scale(R, cosf(a)), t.U, sinf(a))); t.U = v3_norm(madd(v3_scale(t.U, cosf(a)), R, -sinf(a))); break; }
        case T_PUSH:
            if (sp < MAX_STACK) st[sp++] = t;
            t.attachR = t.r; t.run = 0.0f; t.fresh = 1; t.depth++;
            break;
        case T_POP:
            if (sp > 0) t = st[--sp];
            break;
        case T_L: {
            Organ *o = add_organ(c, O_LEAF, &t, m->path, m->i);
            o->shape = m->k;
            memcpy(o->a, m->a, sizeof(o->a));
            break;
        }
        case T_P: { Organ *o = add_organ(c, O_FROND, &t, m->path, m->i); o->shape = LS_KENTIA; memcpy(o->a, m->a, sizeof(o->a)); break; }
        case T_U: { Organ *o = add_organ(c, O_UMBRELLA, &t, m->path, m->i); o->shape = LS_UMBRELLA; memcpy(o->a, m->a, sizeof(o->a)); break; }
        case T_W: { Organ *o = add_organ(c, O_ROOT, &t, m->path, PRIO_FINE); memcpy(o->a, m->a, sizeof(o->a)); break; }
        case T_CLUSTER: { Organ *o = add_organ(c, O_CLUSTER, &t, m->path, m->i); o->shape = m->k; memcpy(o->a, m->a, sizeof(o->a)); break; }
        case T_BT: {
            /* one braided strand: a tapered helix whose orbit closes to the axis at both ends */
            static const int NS[PQ_COUNT] = { 16, 24, 36 };
            int n = NS[c->q], prev = -1;
            float len = m->a[1], ph = m->a[2], turns = m->a[3], orbit = m->a[4], rad = m->a[5];
            vec3 a = t.p;
            for (int k = 1; k <= n; ++k) {
                float u = (float)k / (float)n, ang = 2.0f * PI_F * turns * u + ph, env = powf(maxf(sinf(PI_F * u), 0.0f), 0.7f);
                vec3 b = madd(madd(madd(t.p, t.F, len * u), t.R, orbit * env * cosf(ang)), t.U, orbit * env * sinf(ang));
                float u0 = (float)(k - 1) / (float)n;
                prev = add_seg(c, a, b, rad * (1.0f - 0.3f * u0), rad * (1.0f - 0.3f * u), WM_FICUS, prev, 0, (k == 1 ? SF_SOIL : 0) | SF_FLARE, 0.0f, m->path);
                a = b;
            }
            break;
        }
        case T_BC: {
            int n = c->q == PQ_LOW ? 4 : 6, prev = -1;
            for (int k = 0; k < n; ++k) {
                float u0 = (float)k / (float)n, u1 = (float)(k + 1) / (float)n;
                prev = add_seg(c, madd(t.p, t.F, m->a[0] * u0), madd(t.p, t.F, m->a[0] * u1 + (k == n - 1 ? 0.02f : 0.0f)),
                               m->a[1] * (1.0f - 0.2f * u0), m->a[1] * (1.0f - 0.2f * u1), WM_FICUS, prev, 0, k == 0 ? SF_SOIL | SF_FLARE : SF_FLARE, 0.0f, m->path);
            }
            break;
        }
        case T_ST: {
            static const int NS[PQ_COUNT] = { 14, 22, 32 };
            int n = NS[c->q], prev = -1;
            float len = m->a[0], turns = m->a[1], orbit = m->a[2], ph = m->a[3], rad = m->a[4];
            vec3 a = t.p;
            for (int k = 1; k <= n; ++k) {
                float u = (float)k / (float)n, u0 = (float)(k - 1) / (float)n, ang = 2.0f * PI_F * turns * u + ph, env = powf(maxf(sinf(PI_F * u), 0.0f), 0.7f);
                float x = orbit * env * (cosf(ang) + 0.12f * sinf(2.0f * ang)), z = orbit * env * (0.88f * sinf(ang) + 0.10f * sinf(2.2f * ang));
                vec3 b = madd(madd(madd(t.p, t.F, len * u), t.R, x), t.U, z);
                prev = add_seg(c, a, b, rad * (1.0f - 0.32f * u0) + 0.004f, rad * (1.0f - 0.32f * u) + 0.004f, WM_FICUS, prev, 0, (k == 1 ? SF_SOIL : 0) | SF_FLARE, 0.0f, m->path);
                a = b;
            }
            break;
        }
        case T_J:
            /* the crown starts exactly at the top of the trunk macro */
            t.p = madd(t.p, t.F, m->a[0]);
            t.r = m->a[1]; t.attachR = m->a[1] * 1.6f; t.fresh = 1; t.last = -1; t.run = 0.0f; t.depth = 1;
            break;
        }
    }
}

/* ===========================================================================
 * Space colonisation (stage 2/3 for the woody crowns)
 * ========================================================================= */

typedef struct CNode { vec3 p, d; float r; int seg, kids, colony; vec3 pull; int npull; } CNode;
typedef struct Envelope { vec3 c, r; } Envelope;

static int in_env(const Envelope *e, vec3 p, float grow) {
    float x = (p.x - e->c.x) / (e->r.x * grow), y = (p.y - e->c.y) / (e->r.y * grow), z = (p.z - e->c.z) / (e->r.z * grow);
    return x * x + y * y + z * z <= 1.0f;
}

static void colony_leaves(Ctx *c, const CNode *n, uint32_t path);

void colonize(Ctx *c) {
    static const int POINTS[PQ_COUNT] = { 180, 300, 460 }, CAP[PQ_COUNT] = { 44, 82, 132 }, ITERS[PQ_COUNT] = { 16, 22, 28 };
    float H = c->H, share, step;
    Envelope e;
    if (c->sp == PS_BENJAMINA)      { share = 1.0f;  step = 0.028f * H; e.c = v3(0, 0.80f * H, 0); e.r = v3(0.30f * H * c->W, 0.17f * H, 0.30f * H * c->W); }
    else if (c->sp == PS_LYRATA)    { share = 0.12f; step = 0.06f * H;  e.c = v3(0, 0.70f * H, 0); e.r = v3(0.25f * H * c->W, 0.24f * H, 0.25f * H * c->W); }
    else if (c->sp == PS_SCHEFFLERA){ share = 0.15f; step = 0.035f * H;  e.c = v3(0, 0.66f * H, 0); e.r = v3(0.29f * H * c->W, 0.26f * H, 0.29f * H * c->W); }
    else return;
    float influence = 2.5f * step, kill = 1.2f * step, coll = 0.65f * step;
    int npts = (int)((float)POINTS[c->q] * maxf(share, 0.4f)), cap = (int)((float)CAP[c->q] * share + 0.5f);
    vec3 *pts = (vec3 *)malloc(sizeof(vec3) * (size_t)npts);
    uint8_t *alive = (uint8_t *)malloc((size_t)npts);
    int maxNodes = c->nseg + cap + 8;
    CNode *nd = (CNode *)calloc((size_t)maxNodes, sizeof(CNode));
    if (!pts || !alive || !nd) oom();
    /* deterministic attraction points, only inside the envelope */
    for (int i = 0; i < npts; ++i) {
        vec3 q = v3(0, 0, 0);
        for (int tries = 0; tries < 16; ++tries) {
            uint32_t h = hash_combine(hash_combine(c->key, 0xA77Au), (uint32_t)(i * 16 + tries));
            q = v3(hash_float(hash_combine(h, 1)) * 2.0f - 1.0f, hash_float(hash_combine(h, 2)) * 2.0f - 1.0f, hash_float(hash_combine(h, 3)) * 2.0f - 1.0f);
            if (v3_dot(q, q) <= 1.0f) break;
        }
        pts[i] = v3(e.c.x + q.x * e.r.x, e.c.y + q.y * e.r.y, e.c.z + q.z * e.r.z);
        alive[i] = 1;
    }
    /* the skeleton's woody nodes in (or near) the crown seed the graph */
    int n = 0;
    for (int s = 0; s < c->nseg && n < maxNodes - cap; ++s) {
        const Seg *g = &c->seg[s];
        if ((g->mat != WM_FICUS && g->mat != WM_UMBRELLA) || !in_env(&e, g->b, 1.12f)) continue;
        CNode *q = &nd[n++];
        q->p = g->b; q->d = v3_norm(v3_sub(g->b, g->a)); q->r = g->r1; q->seg = s; q->kids = 1;
    }
    int added = 0;
    for (int it = 0; it < ITERS[c->q] && added < cap && n > 0; ++it) {
        for (int k = 0; k < n; ++k) { nd[k].pull = v3(0, 0, 0); nd[k].npull = 0; }
        for (int i = 0; i < npts; ++i) {
            if (!alive[i]) continue;
            int best = -1;
            float bd = influence * influence;
            for (int k = 0; k < n; ++k) {
                vec3 d = v3_sub(pts[i], nd[k].p);
                float dd = v3_dot(d, d);
                if (dd < bd) { bd = dd; best = k; }
            }
            if (best < 0) continue;
            if (bd < kill * kill) { alive[i] = 0; continue; }
            nd[best].pull = v3_add(nd[best].pull, v3_norm(v3_sub(pts[i], nd[best].p)));
            nd[best].npull++;
        }
        int grew = 0, n0 = n;
        for (int k = 0; k < n0 && added < cap; ++k) {
            if (!nd[k].npull) continue;
            vec3 d = v3_norm(madd(v3_norm(nd[k].pull), nd[k].d, 0.5f));
            float hz = sqrtf(d.x * d.x + d.z * d.z), flex = clampf(1.0f - nd[k].r / (0.01f * H), 0.2f, 1.0f);
            float pd = sqrtf(nd[k].p.x * nd[k].p.x + nd[k].p.z * nd[k].p.z);
            d.y -= c->G * 0.11f * flex * hz * clampf(pd / (0.28f * H), 0.15f, 1.5f);
            d = v3_norm(d);
            vec3 cand = madd(nd[k].p, d, step);
            if (!in_env(&e, cand, 1.0f)) continue;            /* never projected back onto the boundary */
            int ok = 1;
            for (int j = 0; j < n && ok; ++j) {
                if (j == k) continue;
                vec3 dd = v3_sub(cand, nd[j].p);
                ok = v3_dot(dd, dd) >= coll * coll;
            }
            if (!ok) continue;
            float rt = maxf(0.0022f * c->k, minf(nd[k].r, 0.006f * c->k) * 0.85f);
            int cont = nd[k].colony && nd[k].kids == 0;
            int mat = c->seg[nd[k].seg].mat;
            int s = add_seg(c, nd[k].p, cand, cont ? nd[k].r : rt, rt, mat, cont ? nd[k].seg : -1, 2, SF_COLONY, cont ? 0.0f : nd[k].r, child(c->seg[nd[k].seg].path, TAG_X, nd[k].kids));
            nd[k].kids++;
            CNode *q = &nd[n++];
            q->p = cand; q->d = d; q->r = rt; q->seg = s; q->colony = 1; q->kids = 0;
            added++; grew++;
        }
        if (!grew) break;
    }
    for (int k = 0; k < n; ++k) if (nd[k].colony) colony_leaves(c, &nd[k], c->seg[nd[k].seg].path);
    c->out->st.colonized = added;
    free(pts); free(alive); free(nd);
}

/* Supplementary foliage on a colonised node (lower priority than the grammar's). */
static void colony_leaves(Ctx *c, const CNode *n, uint32_t path) {
    Turtle t;
    memset(&t, 0, sizeof(t));
    t.p = n->p; t.F = n->d; t.r = n->r;
    t.R = rot_axis(perp_of(n->d), n->d, rnd(c, path, CH_ROLL) * 2.0f * PI_F);
    t.U = v3_cross(t.R, t.F);
    if (c->sp == PS_BENJAMINA) {
        float hs = c->H / plant_info(PS_BENJAMINA)->refHeight;
        for (int s = 0; s < 2; ++s) {
            uint32_t lp = child(path, TAG_L, s);
            Organ *o = add_organ(c, O_LEAF, &t, lp, PRIO_SUPPLEMENT);
            float roll = (s ? 1.5f : 0.5f) * PI_F + jit(c, lp, CH_AZ, 0.5f), pitch = lerp_r(c, lp, CH_PITCH, 50.0f, 80.0f) * DEG;
            vec3 radial = v3_add(v3_scale(t.R, cosf(roll)), v3_scale(t.U, sinf(roll)));
            o->F = v3_norm(v3_add(v3_scale(t.F, cosf(pitch)), v3_scale(radial, sinf(pitch))));
            o->shape = LS_ELLIPTIC;
            float len = 0.8f * hs * lerp_r(c, lp, CH_LEN, 0.092f, 0.15f);
            o->a[0] = len; o->a[1] = len * lerp_r(c, lp, CH_WID, 0.39f, 0.48f); o->a[2] = 0.0f; o->a[3] = 0.06f; o->a[4] = jit(c, lp, CH_TWIST, 0.5f);
        }
        if (n->kids == 0) {
            Organ *o = add_organ(c, O_CLUSTER, &t, child(path, TAG_C, 0), PRIO_SUPPLEMENT);
            o->shape = LS_ELLIPTIC; o->a[0] = 0.075f * hs; o->a[1] = 0.44f; o->a[2] = 3.0f;
        }
    } else if (c->sp == PS_LYRATA) {
        if (n->kids != 0) return;
        float hs = c->H / plant_info(PS_LYRATA)->refHeight, fl = hs * lerp_r(c, path, CH_LEN, 0.17f, 0.24f);
        Organ *o = add_organ(c, O_LEAF, &t, child(path, TAG_L, 0), PRIO_SUPPLEMENT);
        o->F = v3_norm(madd(t.F, t.U, 0.45f));
        o->shape = LS_FIDDLE;
        o->a[0] = fl; o->a[1] = fl * lerp_r(c, path, CH_WID, 0.65f, 0.8f); o->a[2] = hs * 0.035f; o->a[3] = 0.03f; o->a[4] = jit(c, path, CH_TWIST, 0.3f);
    } else if (c->sp == PS_SCHEFFLERA) {
        if (n->kids != 0) return;
        float hs = c->H / plant_info(PS_SCHEFFLERA)->refHeight, len = hs * lerp_r(c, path, CH_LEN, 0.12f, 0.16f);
        Organ *o = add_organ(c, O_UMBRELLA, &t, child(path, TAG_L, 0), PRIO_SUPPLEMENT);
        o->F = v3_norm(madd(t.F, UP, 0.6f));
        o->shape = LS_UMBRELLA;
        o->a[0] = (float)(7 + (int)(rnd(c, path, CH_COUNT) * 2.99f)); o->a[1] = len; o->a[2] = len * 0.4f;
        o->a[3] = 0.2f; o->a[4] = len * 0.9f; o->a[5] = 0.8f;
    }
}
