#include "plant_internal.h"

/* ===========================================================================
 * Species definitions (section 7)
 * ========================================================================= */

/* ---- 7.1 Kentia palm ---------------------------------------------------- */

/* How spread and old frond i is: 1 the oldest, outermost and most horizontal, 0 the spear. */
static float kentia_pf(const Ctx *c, int i) {
    float base = c->Nf > 1 ? 1.0f - (float)i / (float)(c->Nf - 1) : 0.5f;
    return clampf(0.85f * base + jit(c, node(TAG_C, i), CH_OPEN, 0.18f) + 0.08f, 0.0f, 1.0f);
}

void setup_kentia(Ctx *c, MList *ax) {
    float H = c->H;
    c->Nt = (int)clampf(roundf(H / 0.12f), 10.0f, 22.0f);
    c->Nf = (int)clampf(roundf(8.0f + 2.0f * c->age), 7.0f, 12.0f);
    c->trunkLen = 0.48f * H;         /* the arching crown brings the plant to its height */
    mRot(ax, T_ROLL, rnd(c, TAG_K, CH_AZ) * 2.0f * PI_F);
    mRot(ax, T_PITCH, lerp_r(c, TAG_K, CH_LEAN, 1.0f, 4.0f) * DEG);
    mN(ax, N_TRUNK, 0, TAG_K);
    mN(ax, N_CROWN, 0, TAG_C);
}

void prod_kentia(Ctx *c, const Mod *m, MList *o) {
    float H = c->H;
    int i = m->i;
    uint32_t nd = node(m->path, i);
    if (m->s == N_TRUNK) {
        if (i >= c->Nt) return;
        float t0 = (float)i / (float)c->Nt, t1 = (float)(i + 1) / (float)c->Nt;
        mF(o, c->trunkLen / (float)c->Nt, lerpf(0.024f, 0.014f, t0) * H, lerpf(0.024f, 0.014f, t1) * H, WM_PALM, 0.0f, 0.0f, 0.0f, nd, m->path);
        mN(o, N_TRUNK, i + 1, m->path);
    } else if (m->s == N_CROWN) {
        if (i >= c->Nf) return;
        float pf = kentia_pf(c, i);
        mPush(o);
        mRot(o, T_ROLL, (float)i * GOLDEN + jit(c, nd, CH_AZ, 10.0f * DEG));
        mRot(o, T_PITCH, (10.0f + 62.0f * pf) * DEG + jit(c, nd, CH_PITCH, 4.0f * DEG));
        Mod *p = put(o, T_P);
        float len = H * (0.50f + 0.30f * pf) * (1.0f + jit(c, nd, CH_LEN, 0.07f)) * (0.6f + 0.4f * c->W);
        static const float PAIRS[PQ_COUNT] = { 9.0f, 16.0f, 24.0f };
        p->path = nd; p->i = PRIO_MAJOR;
        p->a[0] = len;
        p->a[1] = 0.32f * len;
        p->a[2] = roundf(PAIRS[c->q] * (0.8f + 0.2f * c->age) * (0.75f + 0.25f * pf));
        p->a[3] = 0.3f + 0.7f * pf;
        p->a[4] = 0.25f + 0.75f * smoothstepf(0.0f, 0.35f, pf);
        mPop(o);
        mN(o, N_CROWN, i + 1, m->path);
    }
}

/* ---- 7.2 Dracaena marginata --------------------------------------------- */

void setup_dracaena(Ctx *c, MList *ax) {
    float H = c->H;
    int n = 3;
    if (c->q == PQ_HIGH) n += rnd(c, TAG_K, CH_COUNT) < 0.35f ? 2 : rnd(c, TAG_K, CH_COUNT) < 0.7f ? 1 : 0;
    else if (c->q == PQ_MEDIUM && rnd(c, TAG_K, CH_COUNT) < 0.5f) n = 4;
    c->canes = n;
    /* heights: independent in 0.55H..H, then the tallest one or two lead */
    float h[5];
    for (int j = 0; j < n; ++j) h[j] = H * lerp_r(c, child(TAG_K, TAG_Z, j), CH_HEIGHT, 0.55f, 0.95f);
    for (int a = 0; a < n; ++a) for (int b = a + 1; b < n; ++b) if (h[b] > h[a]) { float t = h[a]; h[a] = h[b]; h[b] = t; }
    h[0] = H;
    if (n > 2 && rnd(c, TAG_K, CH_LEN) < 0.5f) h[1] = maxf(h[1], 0.88f * H);
    static const int LEAVES[PQ_COUNT] = { 22, 32, 40 };
    for (int j = 0; j < n; ++j) {
        uint32_t cp = child(TAG_K, TAG_Z, j);
        c->caneLen[j] = h[j] * 0.84f;
        c->caneN[j] = (int)clampf(roundf(c->caneLen[j] / 0.09f), 6.0f, 18.0f);
        c->caneR0[j] = H * (0.009f + 0.009f * h[j] / H);
        c->canePhase[j] = rnd(c, cp, CH_PHASE) * 2.0f * PI_F;
        c->caneLeaves[j] = (int)roundf((float)LEAVES[c->q] * (0.8f + 0.2f * c->age) * (0.85f + 0.3f * rnd(c, cp, CH_COUNT)));
        float az = 2.0f * PI_F * (float)j / (float)n + jit(c, cp, CH_AZ, 25.0f * DEG);
        float lean = j == 0 ? lerp_r(c, cp, CH_LEAN, 2.0f, 6.0f) : lerp_r(c, cp, CH_LEAN, 7.0f, 16.0f);
        mPush(ax);
        mRot(ax, T_ROLL, az);
        /* a tiny base-plane offset, so the bases do not coincide */
        mRot(ax, T_PITCH, 90.0f * DEG);
        mf(ax, j == 0 ? 0.004f : lerp_r(c, cp, CH_LEN, 0.025f, 0.05f) * c->k);
        mRot(ax, T_PITCH, -90.0f * DEG);
        mRot(ax, T_PITCH, lean * DEG * (0.6f + 0.4f * c->W));
        Mod *m = mN(ax, N_CANE, 0, cp);
        m->k = (uint8_t)j;
        mPop(ax);
    }
}

void prod_dracaena(Ctx *c, const Mod *m, MList *o) {
    int i = m->i, j = m->k;
    uint32_t nd = node(m->path, i);
    if (m->s == N_CANE) {
        int N = c->caneN[j];
        if (i < N) {
            float t0 = (float)i / (float)N, t1 = (float)(i + 1) / (float)N;
            float r0 = c->caneR0[j] * (1.0f - 0.55f * t0), r1 = c->caneR0[j] * (1.0f - 0.55f * t1);
            mF(o, c->caneLen[j] / (float)N, r0, r1, WM_CANE, 0.25f, 0.5f * c->bv, 0.08f, nd, m->path);
            mRot(o, T_PITCH, jit(c, nd, CH_BEND, 6.0f * DEG) * c->bv / 0.06f);
            mRot(o, T_ROLL, jit(c, nd, CH_ROLL, 25.0f * DEG));
            Mod *n = mN(o, N_CANE, i + 1, m->path); n->k = (uint8_t)j;
        } else {
            Mod *n = mN(o, N_ROSETTE, 0, m->path); n->k = (uint8_t)j;
            n->a[0] = c->caneR0[j] * 0.45f;
        }
    } else if (m->s == N_ROSETTE) {
        int n = c->caneLeaves[j];
        if (i >= n) return;
        float r = m->a[0], f = n > 1 ? (float)i / (float)(n - 1) : 0.0f;
        if (i % 2 == 1) {   /* the stem runs on up through the tuft */
            mF(o, 0.009f * c->k, r, r * 0.96f, WM_CANE, 0.0f, 0.0f, 0.0f, nd, m->path);
            r *= 0.96f;
        }
        float hs = c->H / plant_info(PS_DRACAENA)->refHeight;
        float len = lerpf(0.5f, 0.27f, f) * hs * (1.0f + jit(c, nd, CH_LEN, c->lv));
        mPush(o);
        mRot(o, T_ROLL, (float)i * GOLDEN + c->canePhase[j] + jit(c, nd, CH_AZ, 8.0f * DEG));
        mRot(o, T_PITCH, lerpf(80.0f, 6.0f, powf(f, 0.8f)) * DEG + jit(c, nd, CH_PITCH, 7.0f * DEG));
        mLeaf(o, LS_LINEAR, len, len * lerp_r(c, nd, CH_WID, 0.1f, 0.13f), 0.0f,
              lerpf(0.45f, 0.04f, f), jit(c, nd, CH_TWIST, 0.5f), nd, f < 0.5f ? PRIO_MAJOR : PRIO_LEAF);
        mPop(o);
        Mod *nx = mN(o, N_ROSETTE, i + 1, m->path); nx->k = (uint8_t)j; nx->a[0] = r;
    }
}

/* ---- 7.3 Fiddle-leaf fig ------------------------------------------------- */

/* A few well-spaced branch slots in [lo, hi], at least `gap` apart. */
static uint32_t pick_slots(const Ctx *c, uint32_t path, int lo, int hi, int want, int gap) {
    uint32_t mask = 0;
    int got = 0;
    for (int pass = 0; pass < 4 && got < want; ++pass)
        for (int k = lo; k <= hi && got < want; ++k) {
            if (mask & (1u << k)) continue;
            int ok = rnd(c, node(path, k), CH_SLOT + (uint32_t)pass * 31u) < 0.45f;
            for (int d = 1; d < gap && ok; ++d) ok = !(k - d >= 0 && (mask & (1u << (k - d)))) && !(k + d < 32 && (mask & (1u << (k + d))));
            if (ok) { mask |= 1u << k; got++; }
        }
    return mask;
}

void setup_lyrata(Ctx *c, MList *ax) {
    float H = c->H;
    int Na = (int)clampf(roundf(H / 0.12f), 10.0f, 18.0f);
    c->leaders = 1;
    c->Na[0] = Na;
    c->bare[0] = (int)roundf(0.25f * (float)Na);
    int want = 3 + (int)(rnd(c, TAG_A, CH_COUNT) * 2.99f * c->age);
    int lo = c->bare[0] > (int)(0.4f * (float)Na) ? c->bare[0] : (int)(0.4f * (float)Na);
    c->slots[0] = pick_slots(c, TAG_A, lo, Na - 2, want, 2);
    mN(ax, N_A, 0, TAG_A);
}

void prod_lyrata(Ctx *c, const Mod *m, MList *o) {
    float H = c->H, hs = H / plant_info(PS_LYRATA)->refHeight;
    int k = m->i, Na = c->Na[0];
    uint32_t nd = node(m->path, k);
    float alt = (float)k * (PI_F + 28.0f * DEG);   /* side to side, drifting round the stem */
    if (m->s == N_A) {
        float t = (float)k / (float)Na;
        if (k < Na) {
            float len = H * 0.9f / (float)Na * (1.0f + jit(c, nd, CH_LEN, 0.12f));
            mF(o, len, lerpf(0.025f, 0.009f, t) * H, lerpf(0.025f, 0.009f, (float)(k + 1) / (float)Na) * H, WM_FICUS, 0.18f, 0.35f * c->bv, 0.03f, nd, m->path);
            if (k >= c->bare[0]) {
                float fl = hs * (0.4f - 0.13f * t) * (1.0f + jit(c, nd, CH_LEN, c->lv));
                mPush(o);
                mRot(o, T_ROLL, alt + jit(c, nd, CH_AZ, 14.0f * DEG));
                mRot(o, T_PITCH, (82.0f - 30.0f * t) * DEG + jit(c, nd, CH_PITCH, 8.0f * DEG));
                mLeaf(o, LS_FIDDLE, fl, fl * lerp_r(c, nd, CH_WID, 0.7f, 0.88f), hs * lerp_r(c, nd, CH_PET, 0.035f, 0.055f),
                      0.07f, jit(c, nd, CH_TWIST, 0.2f), nd, PRIO_MAJOR);
                mPop(o);
            }
            if ((c->slots[0] & (1u << k)) && c->branches < c->branchCap) {
                c->branches++;
                mPush(o);
                mRot(o, T_ROLL, (float)k * GOLDEN + jit(c, nd, CH_ROLL, 25.0f * DEG));
                mRot(o, T_PITCH, lerp_r(c, nd, CH_SEC, 36.0f, 56.0f) * DEG);
                Mod *b = mN(o, N_B, 0, child(m->path, TAG_B, k));
                b->a[0] = lerpf(0.025f, 0.009f, t) * H * 0.55f;
                b->a[1] = (float)(4 + (int)(rnd(c, nd, CH_COUNT) * 2.99f * (1.0f - 0.3f * t)));
                b->a[2] = H * 0.11f * c->W * (1.1f - 0.3f * t);
                mPop(o);
            }
            mN(o, N_A, k + 1, m->path);
        } else {
            float fl = hs * 0.17f;
            mPush(o);
            mRot(o, T_ROLL, alt);
            mRot(o, T_PITCH, 16.0f * DEG);
            mLeaf(o, LS_FIDDLE, fl, fl * 0.7f, hs * 0.03f, 0.0f, 0.0f, nd, PRIO_LEAF);
            mPop(o);
        }
    } else if (m->s == N_B) {
        int j = k, nb = (int)m->a[1];
        float r0 = m->a[0], blen = m->a[2];
        if (j < nb) {
            float t0 = (float)j / (float)nb, t1 = (float)(j + 1) / (float)nb;
            mF(o, blen * (1.0f - 0.08f * (float)j), r0 * (1.0f - 0.6f * t0), r0 * (1.0f - 0.6f * t1), WM_FICUS, 0.45f, 0.4f * c->bv, 0.16f, nd, m->path);
            float fl = hs * 0.3f * (1.0f + jit(c, nd, CH_LEN, c->lv));
            mPush(o);
            mRot(o, T_ROLL, (float)j * (PI_F + 30.0f * DEG) + jit(c, nd, CH_AZ, 20.0f * DEG));
            mRot(o, T_PITCH, 62.0f * DEG + jit(c, nd, CH_PITCH, 10.0f * DEG));
            mLeaf(o, LS_FIDDLE, fl, fl * lerp_r(c, nd, CH_WID, 0.7f, 0.86f), hs * 0.04f, 0.07f, jit(c, nd, CH_TWIST, 0.2f), nd, PRIO_LEAF);
            mPop(o);
            Mod *b = mN(o, N_B, j + 1, m->path);
            memcpy(b->a, m->a, sizeof(b->a));
        } else {
            float fl = hs * 0.15f;
            mPush(o);
            mRot(o, T_PITCH, 12.0f * DEG);
            mLeaf(o, LS_FIDDLE, fl, fl * 0.7f, hs * 0.025f, 0.0f, 0.0f, nd, PRIO_LEAF);
            mPop(o);
        }
    }
}

/* ---- 7.4 Weeping fig ----------------------------------------------------- */

static int sec_on(const Ctx *c, int prim, int slot) { return rnd(c, child(child(TAG_P, TAG_P, prim), TAG_S, slot), CH_SEC) < c->pSec; }
static int ter_on(const Ctx *c, int prim, int slot) { return rnd(c, child(child(TAG_P, TAG_P, prim), TAG_S, slot), CH_TER) < c->pTer; }

/* Conservative upper bound of leaf placements and the branch count for the current probabilities. */
static void benjamina_count(const Ctx *c, int *leaves, int *branches) {
    int L = 2 * 3 + 6, B = 0;   /* the leader's top leaves and cluster */
    for (int p = 0; p < c->nprim; ++p) {
        L += c->Np * 3 + 5; B++;
        for (int s = 0; s < c->nsec; ++s) {
            if (!sec_on(c, p, s)) continue;
            L += c->Ns * 3 + 5; B++;
            if (ter_on(c, p, s)) { L += c->Ntt * 3 + 5; B++; }
        }
    }
    *leaves = L; *branches = B;
}

void setup_benjamina(Ctx *c, MList *ax) {
    float H = c->H, hs = H / plant_info(PS_BENJAMINA)->refHeight;
    static const int NP[PQ_COUNT] = { 7, 11, 14 }, NS[PQ_COUNT] = { 4, 7, 10 }, NT[PQ_COUNT] = { 3, 5, 7 };
    static const int BRANCH_CAP[PQ_COUNT] = { 14, 50, 60 }, FOLIAGE_CAP[PQ_COUNT] = { 260, 1000, 1800 };
    static const int SLOTS[PQ_COUNT][5] = { { 2, 4 }, { 1, 3, 5, 7, 9 }, { 1, 4, 7, 10, 12 } };
    c->form = c->P.form >= 0 ? c->P.form : (rnd(c, TAG_X, CH_FORM) < 0.72f ? 0 : 1);
    c->Np = NP[c->q]; c->Ns = NS[c->q]; c->Ntt = NT[c->q];
    c->nsec = c->q == PQ_LOW ? 2 : 5;
    memcpy(c->secSlot, SLOTS[c->q], sizeof(c->secSlot));
    c->branchCap = BRANCH_CAP[c->q];
    c->foliageCap = FOLIAGE_CAP[c->q];
    c->Ncr = c->q == PQ_LOW ? 5 : c->q == PQ_MEDIUM ? 6 : 7;
    c->crownLen = 0.17f * H;
    c->nprim = (c->q == PQ_LOW ? 5 : 5 + (int)(rnd(c, TAG_P, CH_COUNT) * 2.99f));
    for (int p = 0; p < c->nprim; ++p) {
        uint32_t pp = child(TAG_P, TAG_P, p);
        c->primNode[p] = (int)clampf(1.0f + floorf((float)p * (float)(c->Ncr - 1) / (float)c->nprim), 1.0f, (float)(c->Ncr - 1));
        c->primYaw[p] = (float)p * GOLDEN + jit(c, pp, CH_AZ, 22.0f * DEG);
        /* lower limbs launch wider, upper ones more upright, so the crown domes */
        c->primPitch[p] = lerpf(0.95f, 0.5f, (float)p / (float)(c->nprim - 1)) + jit(c, pp, CH_PITCH, 0.1f);
        c->primLen[p] = H * lerp_r(c, pp, CH_LEN, 0.36f, 0.46f) * lerpf(c->W, 1.0f, (float)p / (float)c->nprim * 0.5f) * (1.0f - 0.3f * (float)p / (float)c->nprim);
    }
    /* budget before detail: thin tertiaries, then secondaries, until the bound fits */
    c->pSec = 0.55f + 0.3f * c->age;
    c->pTer = c->q == PQ_LOW ? 0.3f : c->q == PQ_MEDIUM ? 0.6f : 0.8f;
    int L, B;
    benjamina_count(c, &L, &B);
    while ((L > c->foliageCap || B > c->branchCap) && c->pTer > 0.0f) { c->pTer -= 0.1f; benjamina_count(c, &L, &B); }
    while ((L > c->foliageCap || B > c->branchCap) && c->pSec > 0.0f) { c->pSec -= 0.1f; benjamina_count(c, &L, &B); }

    float trunk = (c->form == 0 ? 0.62f : 0.64f) * H;
    if (c->form == 0) {
        c->Nbt = (int)clampf(roundf(3.0f + 2.0f * c->age), 4.0f, 5.0f);
        for (int j = 0; j < c->Nbt; ++j) {
            uint32_t sp = child(TAG_X, TAG_S, j);
            Mod *m = put(ax, T_BT);
            m->i = (uint16_t)j; m->path = sp;
            m->a[0] = (float)c->Nbt;
            m->a[1] = trunk;
            m->a[2] = 2.0f * PI_F * (float)j / (float)c->Nbt + jit(c, sp, CH_PHASE, 0.12f);
            m->a[3] = 2.05f + jit(c, sp, CH_TURNS, 0.17f);
            m->a[4] = lerp_r(c, sp, CH_ORBIT, 0.025f, 0.029f) * hs;
            m->a[5] = lerp_r(c, sp, CH_RAD, 0.0165f, 0.019f) * hs;
        }
        Mod *core = put(ax, T_BC);
        core->a[0] = trunk; core->a[1] = lerp_r(c, TAG_X, CH_RAD, 0.013f, 0.014f) * hs;
        core->path = child(TAG_X, TAG_C, 0);
    } else {
        Mod *m = put(ax, T_ST);
        m->path = child(TAG_X, TAG_T, 0);
        m->a[0] = trunk;
        m->a[1] = lerp_r(c, TAG_X, CH_TURNS, 1.4f, 1.7f);
        m->a[2] = lerp_r(c, TAG_X, CH_ORBIT, 0.044f, 0.054f) * hs;
        m->a[3] = rnd(c, TAG_X, CH_PHASE) * 2.0f * PI_F;
        m->a[4] = 0.026f * hs;
    }
    Mod *j = put(ax, T_J);
    j->a[0] = trunk;
    j->a[1] = 0.015f * hs;
    mN(ax, N_CROWN, 0, TAG_C);
}

/* Two opposing sprays, a third leaf on selected nodes. */
static void benjamina_leaves(Ctx *c, MList *o, uint32_t nd, float size, int third, int prio) {
    float hs = c->H / plant_info(PS_BENJAMINA)->refHeight;
    int n = 2 + (third ? 1 : 0);
    for (int s = 0; s < n; ++s) {
        uint32_t lp = child(nd, TAG_L, s);
        float len = size * hs * lerp_r(c, lp, CH_LEN, 0.092f, 0.15f) * (1.0f + jit(c, lp, CH_WID, c->lv * 0.5f));
        mPush(o);
        mRot(o, T_ROLL, s < 2 ? (s ? 1.5f * PI_F : 0.5f * PI_F) + jit(c, lp, CH_AZ, 30.0f * DEG) : rnd(c, lp, CH_AZ) * 2.0f * PI_F);
        mRot(o, T_PITCH, lerp_r(c, lp, CH_PITCH, 50.0f, 80.0f) * DEG);
        mLeaf(o, LS_ELLIPTIC, len, len * lerp_r(c, lp, CH_WID, 0.39f, 0.48f), 0.006f * hs, 0.06f, jit(c, lp, CH_TWIST, 0.5f), lp, prio);
        mPop(o);
    }
}

static void benjamina_twig(Ctx *c, MList *o, const Mod *m, int sym, int n, float total, float r0, float bend, int third) {
    int i = m->i;
    uint32_t nd = node(m->path, i);
    float t0 = (float)i / (float)n, t1 = (float)(i + 1) / (float)n;
    float rt = 0.0022f * c->k;
    mF(o, total / (float)n * (1.0f + jit(c, nd, CH_LEN, 0.15f)), maxf(r0 * (1.0f - 0.75f * t0), rt), maxf(r0 * (1.0f - 0.75f * t1), rt),
       WM_FICUS, 0.75f, 0.5f * c->bv, 0.0f, nd, m->path);
    float size = sym == N_PRIMARY ? 1.0f : sym == N_SECONDARY ? 0.88f : 0.78f;
    benjamina_leaves(c, o, nd, size, third, sym == N_PRIMARY ? PRIO_LEAF : PRIO_FINE);
    /* rising, then curving over: the outer section arcs down, the last four internodes droop */
    float arc = t0 > 0.4f ? bend * (0.6f + (t0 - 0.4f) * 1.6f) : 0.0f;
    if (i >= n - 4) arc += bend * 0.5f;
    mRot(o, T_PITCH, arc * DEG + jit(c, nd, CH_BEND, 3.0f * DEG));
    mRot(o, T_ROLL, jit(c, nd, CH_ROLL, 8.0f * DEG));
}

void prod_benjamina(Ctx *c, const Mod *m, MList *o) {
    float hs = c->H / plant_info(PS_BENJAMINA)->refHeight;
    int i = m->i;
    uint32_t nd = node(m->path, i);
    if (m->s == N_CROWN) {
        if (i >= c->Ncr) { mCluster(o, LS_ELLIPTIC, 0.085f * hs, 0.44f, 5, nd); return; }
        float t0 = (float)i / (float)c->Ncr, t1 = (float)(i + 1) / (float)c->Ncr;
        float rb = 0.015f * hs, rt = 0.006f * hs;
        mF(o, c->crownLen / (float)c->Ncr, lerpf(rb, rt, t0), lerpf(rb, rt, t1), WM_FICUS, 0.3f, 0.3f * c->bv, 0.05f, nd, m->path);
        for (int p = 0; p < c->nprim; ++p) {
            if (c->primNode[p] != i || c->branches >= c->branchCap) continue;
            c->branches++;
            mPush(o);
            mRot(o, T_ROLL, c->primYaw[p]);
            mRot(o, T_PITCH, c->primPitch[p]);
            Mod *pm = mN(o, N_PRIMARY, 0, child(TAG_P, TAG_P, p));
            pm->k = (uint8_t)p;
            pm->a[0] = lerpf(rb, rt, t1) * 0.8f;
            mPop(o);
        }
        if (i >= c->Ncr - 2) benjamina_leaves(c, o, nd, 0.9f, 1, PRIO_LEAF);
        mN(o, N_CROWN, i + 1, m->path);
    } else if (m->s == N_PRIMARY) {
        int p = m->k;
        if (i >= c->Np) { mCluster(o, LS_ELLIPTIC, 0.095f * hs, 0.44f, 4 + (int)(rnd(c, nd, CH_COUNT) * 2.0f), nd); return; }
        benjamina_twig(c, o, m, N_PRIMARY, c->Np, c->primLen[p], m->a[0], 6.5f, rnd(c, nd, CH_THIRD) < 0.45f);
        if (i == 1 && c->q != PQ_LOW && rnd(c, nd, CH_ROOT) < 0.3f) {   /* now and then an aerial root hangs from a limb */
            mPush(o);
            mRot(o, T_PITCH, 70.0f * DEG);
            mRoot(o, hs * lerp_r(c, nd, CH_LEN, 0.12f, 0.25f), 0.45f, 0.0016f * hs, child(nd, TAG_R, 0));
            mPop(o);
        }
        for (int s = 0; s < c->nsec; ++s) {
            if (c->secSlot[s] != i || !sec_on(c, p, s) || c->branches >= c->branchCap) continue;
            c->branches++;
            uint32_t sp = child(m->path, TAG_S, s);
            mPush(o);
            mRot(o, T_ROLL, ((s & 1) ? 0.5f : 1.5f) * PI_F + jit(c, sp, CH_AZ, 30.0f * DEG));
            mRot(o, T_PITCH, lerp_r(c, sp, CH_PITCH, 35.0f, 55.0f) * DEG);
            Mod *sm = mN(o, N_SECONDARY, 0, sp);
            sm->k = (uint8_t)p; sm->i = 0;
            sm->a[0] = m->a[0] * (1.0f - 0.75f * (float)i / (float)c->Np) * 0.7f;
            sm->a[1] = (float)s;
            sm->a[2] = c->primLen[p] * lerp_r(c, sp, CH_LEN, 0.48f, 0.62f);
            sm->a[3] = (float)(1 + (int)(rnd(c, sp, CH_NODE) * (float)(c->Ns - 2)));   /* the tertiary's node */
            mPop(o);
        }
        Mod *n = mN(o, N_PRIMARY, i + 1, m->path);
        n->k = m->k; memcpy(n->a, m->a, sizeof(n->a));
    } else if (m->s == N_SECONDARY) {
        int p = m->k, s = (int)m->a[1];
        if (i >= c->Ns) { mCluster(o, LS_ELLIPTIC, 0.085f * hs, 0.44f, 3 + (int)(rnd(c, nd, CH_COUNT) * 2.0f), nd); return; }
        benjamina_twig(c, o, m, N_SECONDARY, c->Ns, m->a[2], m->a[0], 6.0f, rnd(c, nd, CH_THIRD) < 0.4f);
        if (i == (int)m->a[3] && ter_on(c, p, s) && c->branches < c->branchCap) {
            c->branches++;
            uint32_t tp = child(m->path, TAG_T, i);
            mPush(o);
            mRot(o, T_ROLL, (rnd(c, tp, CH_AZ) < 0.5f ? 0.5f : 1.5f) * PI_F + jit(c, tp, CH_ROLL, 25.0f * DEG));
            mRot(o, T_PITCH, lerp_r(c, tp, CH_PITCH, 35.0f, 50.0f) * DEG);
            Mod *tm = mN(o, N_TERTIARY, 0, tp);
            tm->a[0] = m->a[0] * 0.6f;
            tm->a[2] = m->a[2] * lerp_r(c, tp, CH_LEN, 0.55f, 0.7f);
            mPop(o);
        }
        Mod *n = mN(o, N_SECONDARY, i + 1, m->path);
        n->k = m->k; memcpy(n->a, m->a, sizeof(n->a));
    } else if (m->s == N_TERTIARY) {
        if (i >= c->Ntt) { mCluster(o, LS_ELLIPTIC, 0.075f * hs, 0.44f, 3, nd); return; }
        benjamina_twig(c, o, m, N_TERTIARY, c->Ntt, m->a[2], m->a[0], 5.0f, c->q == PQ_HIGH && (i & 1));
        Mod *n = mN(o, N_TERTIARY, i + 1, m->path);
        memcpy(n->a, m->a, sizeof(n->a));
    }
}

/* ---- 7.5 Dwarf umbrella tree --------------------------------------------- */

void setup_schefflera(Ctx *c, MList *ax) {
    float H = c->H, r = rnd(c, TAG_A, CH_COUNT);
    c->leaders = r < 0.3f ? 1 : r < 0.7f ? 2 : 3;
    for (int l = 0; l < c->leaders; ++l) {
        uint32_t lp = child(TAG_A, TAG_A, l);
        float h = l == 0 ? 1.0f : l == 1 ? lerp_r(c, lp, CH_HEIGHT, 0.75f, 0.9f) : lerp_r(c, lp, CH_HEIGHT, 0.55f, 0.75f);
        c->leaderLen[l] = H * 0.84f * h;
        c->Na[l] = (int)clampf(roundf(c->leaderLen[l] / 0.08f), 8.0f, 16.0f);
        c->bare[l] = (int)roundf(0.22f * (float)c->Na[l]);
        int want = 2 + (int)(rnd(c, lp, CH_SLOT) * 2.99f * c->age);
        c->slots[l] = pick_slots(c, lp, (int)(0.35f * (float)c->Na[l]), c->Na[l] - 2, want, 2);
        mPush(ax);
        mRot(ax, T_ROLL, l == 0 ? 0.0f : (float)l * 2.2f + jit(c, lp, CH_AZ, 0.4f));
        mRot(ax, T_PITCH, (l == 0 ? lerp_r(c, lp, CH_LEAN, 0.0f, 4.0f) : lerp_r(c, lp, CH_LEAN, 7.0f, 15.0f)) * DEG);
        Mod *m = mN(ax, N_A, 0, lp);
        m->k = (uint8_t)l;
        mPop(ax);
    }
}

static void schefflera_umbrella(Ctx *c, MList *o, uint32_t nd, float t, float size) {
    float hs = c->H / plant_info(PS_SCHEFFLERA)->refHeight;
    float cr = rnd(c, nd, CH_COUNT);
    int n = cr < 0.05f ? 6 : cr < 0.1f ? 10 : 7 + (int)(rnd(c, nd, CH_SLOT) * 2.99f);
    float len = size * hs * lerpf(0.24f, 0.175f, t) * (1.0f + jit(c, nd, CH_LEN, c->lv));
    Mod *u = put(o, T_U);
    u->path = nd; u->i = PRIO_MAJOR;
    u->a[0] = (float)n;
    u->a[1] = len;
    u->a[2] = len * lerp_r(c, nd, CH_WID, 0.36f, 0.44f);
    u->a[3] = lerp_r(c, nd, CH_DROOP, 0.1f, 0.3f);
    u->a[4] = len * lerp_r(c, nd, CH_PET, 0.8f, 1.2f);
    u->a[5] = clampf(0.5f + 0.5f * (1.0f - t * t) + jit(c, nd, CH_OPEN, 0.15f), 0.3f, 1.0f);
}

void prod_schefflera(Ctx *c, const Mod *m, MList *o) {
    float H = c->H;
    int k = m->i, l = m->k, Na = c->Na[l];
    uint32_t nd = node(m->path, k);
    if (m->s == N_A) {
        if (k >= Na) return;
        float t0 = (float)k / (float)Na, t1 = (float)(k + 1) / (float)Na, rb = 0.019f * H * (l ? 0.8f : 1.0f);
        mF(o, c->leaderLen[l] / (float)Na * (1.0f + jit(c, nd, CH_LEN, 0.12f)), rb * (1.0f - 0.65f * t0), rb * (1.0f - 0.65f * t1),
           WM_UMBRELLA, 0.2f, 0.4f * c->bv, 0.04f, nd, m->path);
        if (k >= c->bare[l]) {
            mPush(o);
            mRot(o, T_ROLL, (float)k * GOLDEN + (float)l + jit(c, nd, CH_AZ, 15.0f * DEG));
            mRot(o, T_PITCH, (52.0f - 22.0f * t0) * DEG + jit(c, nd, CH_PITCH, 8.0f * DEG));
            schefflera_umbrella(c, o, nd, t0, 1.0f);
            mPop(o);
        }
        if ((c->slots[l] & (1u << k)) && c->branches < c->branchCap) {
            c->branches++;
            uint32_t bp = child(m->path, TAG_B, k);
            mPush(o);
            mRot(o, T_ROLL, (float)k * GOLDEN + 1.3f + jit(c, bp, CH_AZ, 25.0f * DEG));
            mRot(o, T_PITCH, lerp_r(c, bp, CH_PITCH, 35.0f, 55.0f) * DEG);
            Mod *b = mN(o, N_B, 0, bp);
            b->a[0] = rb * (1.0f - 0.65f * t1) * 0.6f;
            b->a[1] = (float)(3 + (int)(rnd(c, bp, CH_COUNT) * 2.99f));
            b->a[2] = H * 0.09f * c->W;
            b->a[3] = t0;
            mPop(o);
        }
        Mod *n = mN(o, N_A, k + 1, m->path); n->k = m->k;
    } else if (m->s == N_B) {
        int nb = (int)m->a[1];
        if (k >= nb) return;
        float t0 = (float)k / (float)nb, t1 = (float)(k + 1) / (float)nb;
        mF(o, m->a[2], m->a[0] * (1.0f - 0.55f * t0), m->a[0] * (1.0f - 0.55f * t1), WM_UMBRELLA, 0.45f, 0.4f * c->bv, 0.14f, nd, m->path);
        mPush(o);
        mRot(o, T_ROLL, (float)k * GOLDEN + jit(c, nd, CH_AZ, 20.0f * DEG));
        mRot(o, T_PITCH, lerp_r(c, nd, CH_PITCH, 30.0f, 55.0f) * DEG);
        schefflera_umbrella(c, o, nd, clampf(m->a[3] + 0.2f * t1, 0.0f, 1.0f), 0.9f);
        mPop(o);
        Mod *n = mN(o, N_B, k + 1, m->path); memcpy(n->a, m->a, sizeof(n->a));
    }
}

/* ---- 7.6 ZZ plant -------------------------------------------------------- */

typedef struct Stalk { float len, lean, phase; int bare, leaves; } Stalk;

static Stalk zz_stalk(const Ctx *c, int s) {
    Stalk st;
    uint32_t sp = child(TAG_Z, TAG_Z, s);
    float h = s == 0 ? 0.95f : lerp_r(c, sp, CH_HEIGHT, 0.35f, 0.9f);
    st.len = c->H * h;
    st.lean = (8.0f + 26.0f * (1.0f - h) + jit(c, sp, CH_LEAN, 5.0f)) * DEG * (0.7f + 0.3f * c->W);
    st.phase = (float)s * GOLDEN + jit(c, sp, CH_AZ, 18.0f * DEG);
    st.bare = 3;
    st.leaves = (int)clampf(roundf(6.0f + 6.0f * (h - 0.35f) / 0.55f + jit(c, sp, CH_COUNT, 1.0f)), 6.0f, 12.0f);
    return st;
}

void setup_zz(Ctx *c, MList *ax) {
    static const float STALKS[PQ_COUNT] = { 8.0f, 12.0f, 15.0f };
    c->stalks = (int)clampf(roundf(STALKS[c->q] * (0.75f + 0.25f * c->age)), 8.0f, 16.0f);
    for (int s = 0; s < c->stalks; ++s) {
        Stalk st = zz_stalk(c, s);
        mPush(ax);
        mRot(ax, T_ROLL, st.phase);
        mRot(ax, T_PITCH, 90.0f * DEG);
        mf(ax, lerp_r(c, child(TAG_Z, TAG_Z, s), CH_ORBIT, 0.0f, 0.06f) * c->k);
        mRot(ax, T_PITCH, -90.0f * DEG);
        mRot(ax, T_PITCH, st.lean);
        Mod *m = mN(ax, N_Z, 0, child(TAG_Z, TAG_Z, s));
        m->k = (uint8_t)s;
        mPop(ax);
    }
}

void prod_zz(Ctx *c, const Mod *m, MList *o) {
    int k = m->i;
    Stalk st = zz_stalk(c, m->k);
    int n = st.bare + st.leaves;
    if (k >= n) return;
    uint32_t nd = node(m->path, k);
    float hs = c->H / plant_info(PS_ZZ)->refHeight;
    /* a bare, swollen petiole base, then the leaflet rows, internodes shortening to the tip */
    float seg = k < st.bare ? st.len * 0.32f / (float)st.bare
              : st.len * 0.68f / (float)st.leaves * (1.25f - 0.5f * (float)(k - st.bare) / (float)st.leaves);
    float t0 = (float)k / (float)n, t1 = (float)(k + 1) / (float)n;
    float rb = 0.011f * hs, r0 = rb * (1.0f - 0.7f * t0), r1 = rb * (1.0f - 0.7f * t1);
    if (k == 0) r0 = rb * 1.7f;
    mF(o, seg, r0, r1, WM_ZZ, 0.3f, 0.0f, 0.0f, nd, m->path);
    if (k >= st.bare) {
        int j = k - st.bare;
        float f = (float)j / (float)maxf((float)(st.leaves - 1), 1.0f);
        float len = hs * lerp_r(c, m->path, CH_LEN, 0.12f, 0.19f) * (0.62f + 0.38f * sinf(PI_F * (0.25f + 0.75f * f))) * (1.0f + jit(c, nd, CH_LEN, c->lv * 0.6f));
        mPush(o);
        mRot(o, T_ROLL, ((j & 1) ? 0.5f : 1.5f) * PI_F + (float)j * 8.0f * DEG + jit(c, nd, CH_AZ, 10.0f * DEG));
        mRot(o, T_PITCH, lerp_r(c, nd, CH_PITCH, 55.0f, 75.0f) * DEG);
        mLeaf(o, LS_ZZ, len, len * lerp_r(c, nd, CH_WID, 0.48f, 0.58f), 0.0f, 0.02f, jit(c, nd, CH_TWIST, 0.25f), nd, PRIO_LEAF);
        mPop(o);
    }
    /* curving outward as it rises */
    mRot(o, T_PITCH, (1.5f + 2.5f * t0) * DEG + jit(c, nd, CH_BEND, 1.5f * DEG));
    Mod *nx = mN(o, N_Z, k + 1, m->path);
    nx->k = m->k;
}
