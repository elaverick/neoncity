#include "plant.h"
#include "hash.h"
#include "mathlib.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ===========================================================================
 * Lobby plants (LPLS-1.19). See src/plant.h for the outline. The stages run
 * in the specification's order:
 *
 *   1. resolve parameters and the crown envelope        (setup_*)
 *   2. expand the bounded L-system                      (expand, prod_*)
 *   3. interpret it with the 3D turtle: stem segments,  (interpret)
 *      organ descriptors, gravity sag and coherent
 *      centre-line noise on the woody axes
 *   4. space colonisation in the woody crowns           (colonize)
 *   5. budgets: estimate, prune supplements first       (budget)
 *   6. organ geometry: leaves, fronds, umbrellas,       (emit_organs, emit_tubes)
 *      petioles and roots; tapered tubes for the stems
 *   7. crown occlusion, bounds and validation           (occlusion, validate)
 *
 * Modules are typed records rather than characters: a symbol, a few float
 * parameters and a stable node path. Productions are applied to every
 * non-terminal of the current list in parallel, in order, so the result
 * keeps the bracket structure of the axiom. Every random choice is
 * rnd(node path, channel), keyed by the seed and the species.
 * ========================================================================= */

#define DEG        (PI_F / 180.0f)
#define GOLDEN     (137.5078f * DEG)
#define MAX_PASSES 48
#define MAX_MODS   400000
#define MAX_STACK  64
#define UP         v3(0.0f, 1.0f, 0.0f)

static const PlantInfo INFO[PS_COUNT] = {
    { "kentia_palm",           "Kentia palm",            1.8f, 1.00f },
    { "dracaena_marginata",    "Madagascar dragon tree", 1.7f, 0.50f },
    { "ficus_lyrata",          "Fiddle-leaf fig",        1.8f, 0.48f },
    { "ficus_benjamina",       "Weeping fig",            2.0f, 0.62f },
    { "schefflera_arboricola", "Dwarf umbrella tree",    1.7f, 0.58f },
    { "zamioculcas",           "ZZ plant",               1.0f, 0.90f },
};

const PlantInfo *plant_info(int species) { return &INFO[species < 0 || species >= PS_COUNT ? 0 : species]; }

void plant_defaults(PlantParams *p, int species, uint32_t seed) {
    memset(p, 0, sizeof(*p));
    p->species = species;
    p->seed = seed;
    p->height = plant_info(species)->refHeight;
    p->widthScale = 1.0f;
    p->age = 0.92f;
    p->gravity = 0.85f;
    p->physics = 1;
    p->quality = PQ_MEDIUM;
    p->leafVariation = 0.12f;
    p->bendVariation = 0.06f;
    p->tintVariation = 0.04f;
    p->form = -1;
    p->floorDepth = 0.5f;
}

/* ------------------------------------------------------------- vectors --- */

static vec3 madd(vec3 a, vec3 b, float s) { return v3(a.x + b.x * s, a.y + b.y * s, a.z + b.z * s); }
/* Rodrigues: v turned by angle a about unit axis k. */
static vec3 rot_axis(vec3 v, vec3 k, float a) {
    float c = cosf(a), s = sinf(a);
    return v3_add(v3_add(v3_scale(v, c), v3_scale(v3_cross(k, v), s)), v3_scale(k, v3_dot(k, v) * (1.0f - c)));
}
static vec3 perp_of(vec3 d) {
    vec3 a = fabsf(d.y) < 0.9f ? UP : v3(1.0f, 0.0f, 0.0f);
    return v3_norm(v3_cross(d, a));
}
static int finite3(vec3 v) { return isfinite(v.x) && isfinite(v.y) && isfinite(v.z); }

/* ------------------------------------------------------------- modules --- */

enum {
    /* terminals: turtle commands and organ modules */
    T_F = 0, T_f, T_YAW, T_PITCH, T_ROLL, T_PUSH, T_POP, T_L, T_P, T_U, T_W, T_BT, T_BC, T_ST, T_J, T_CLUSTER,
    /* non-terminals */
    N_FIRST, N_TRUNK = N_FIRST, N_CROWN, N_CANE, N_ROSETTE, N_A, N_B, N_PRIMARY, N_SECONDARY, N_TERTIARY, N_Z
};

/* F: a = length, r0, r1, flexibility, noise, tropism; k = material; axis = the axis it belongs to.
 * L: a = length, width, petiole, droop, twist; k = shape; i = priority.
 * P: a = length, leaflet length, leaflets per side, droop, openness.
 * U: a = leaflets, leaflet length, width, droop, petiole, openness.
 * W: a = length, curvature, radius.
 * CLUSTER: a = leaf length, width / length, count; k = shape. */
typedef struct Mod {
    uint8_t  s, k;
    uint16_t i;
    uint32_t path, axis;
    float    a[6];
} Mod;

typedef struct MList { Mod *m; int n, cap; } MList;

static void oom(void) { fprintf(stderr, "FATAL: out of memory growing a plant\n"); exit(1); }

static Mod *put(MList *l, int s) {
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 1024;
        l->m = (Mod *)realloc(l->m, sizeof(Mod) * (size_t)l->cap);
        if (!l->m) oom();
    }
    Mod *m = &l->m[l->n++];
    memset(m, 0, sizeof(*m));
    m->s = (uint8_t)s;
    return m;
}

/* -------------------------------------------------------------- output --- */

enum { SF_SOIL = 1, SF_COLONY = 2, SF_FLARE = 4, SF_CAP = 8 };

/* A tapered stem segment. prev: the segment this one continues (they share a
 * ring), or -1 if it starts a new axis (it then reaches back into its parent
 * by attachR so the joint overlaps). */
typedef struct Seg {
    vec3     a, b;
    float    r0, r1, attachR;
    int      prev;
    uint8_t  mat, flags, depth, sides;
    uint32_t path;
} Seg;

enum { O_LEAF = 0, O_FROND, O_UMBRELLA, O_ROOT, O_CLUSTER };
enum { PRIO_MAJOR = 0, PRIO_LEAF, PRIO_FINE, PRIO_SUPPLEMENT };

typedef struct Organ {
    uint8_t  type, shape, prio, flags;
    uint32_t path;
    vec3     p, F, R, U;
    float    stemR;
    float    a[6];
} Organ;

typedef struct Turtle {
    vec3  p, F, R, U;
    float r, run;          /* current stem radius; branchRun: length since this axis's PUSH */
    int   last, fresh, depth;
    float attachR;
} Turtle;

typedef struct Ctx {
    PlantParams P;
    int      sp, q;
    uint32_t key;
    float    H, k, W, age, G, lv, bv;
    MList    cur, nxt;
    int      passes;

    Seg     *seg;  int nseg, capseg;
    Organ   *org;  int norg, caporg;
    int      branches, branchCap;
    int      detail;            /* blade/tube detail reduction steps (budget) */
    float    leafletKeep;       /* share of compound leaflets kept (budget) */

    /* kentia */
    int   Nt, Nf;
    float trunkLen;
    /* dracaena */
    int   canes, caneN[5], caneLeaves[5];
    float caneLen[5], caneR0[5], canePhase[5];
    /* lyrata, schefflera: per leader */
    int      leaders, Na[3], bare[3];
    float    leaderLen[3];
    uint32_t slots[3];
    /* benjamina */
    int   form, Nbt, Ncr, Np, Ns, Ntt, nprim, primNode[8], secSlot[5], nsec, foliageCap;
    float primYaw[8], primPitch[8], primLen[8], pSec, pTer, crownLen;
    /* zz */
    int   stalks;

    PlantMesh *out;
} Ctx;

/* ---------------------------------------------------------- randomness --- */

enum { CH_AZ = 1, CH_PITCH, CH_LEN, CH_WID, CH_TWIST, CH_DROOP, CH_BEND, CH_ROLL, CH_SLOT, CH_COUNT, CH_PHASE,
       CH_TURNS, CH_ORBIT, CH_TINT, CH_FORM, CH_HEIGHT, CH_THIRD, CH_SEC, CH_TER, CH_NODE, CH_LEAN, CH_OPEN, CH_ATTR,
       CH_PET, CH_RAD, CH_NOISE, CH_ROOT };
enum { TAG_A = 0x41, TAG_B = 0x42, TAG_C = 0x43, TAG_P = 0x50, TAG_S = 0x53, TAG_T = 0x54, TAG_Z = 0x5A,
       TAG_R = 0x52, TAG_K = 0x4B, TAG_X = 0x58, TAG_L = 0x4C };

static uint32_t child(uint32_t path, uint32_t tag, int i) { return hash_combine(hash_combine(path, tag), (uint32_t)i); }
static uint32_t node(uint32_t axis, int i) { return hash_combine(axis ^ 0x9E3779B9u, (uint32_t)i); }
/* rand(node, channel) in [0, 1) */
static float rnd(const Ctx *c, uint32_t path, uint32_t ch) { return hash_float(hash_combine(hash_combine(c->key, path), ch)); }
/* jitter(node, channel, amplitude) in [-amp, amp] */
static float jit(const Ctx *c, uint32_t path, uint32_t ch, float amp) { return (rnd(c, path, ch) * 2.0f - 1.0f) * amp; }
static float lerp_r(const Ctx *c, uint32_t path, uint32_t ch, float lo, float hi) { return lo + (hi - lo) * rnd(c, path, ch); }

/* Value noise, keyed: a smooth field in [0, 1). */
static float vnoise3(uint32_t key, vec3 p) {
    float fx = floorf(p.x), fy = floorf(p.y), fz = floorf(p.z);
    int ix = (int)fx, iy = (int)fy, iz = (int)fz;
    float tx = p.x - fx, ty = p.y - fy, tz = p.z - fz;
    tx = tx * tx * (3.0f - 2.0f * tx); ty = ty * ty * (3.0f - 2.0f * ty); tz = tz * tz * (3.0f - 2.0f * tz);
    float v[8];
    for (int k = 0; k < 8; ++k) v[k] = hash_float(hash3i(key, ix + (k & 1), iy + ((k >> 1) & 1), iz + (k >> 2)));
    float a = lerpf(lerpf(v[0], v[1], tx), lerpf(v[2], v[3], tx), ty);
    float b = lerpf(lerpf(v[4], v[5], tx), lerpf(v[6], v[7], tx), ty);
    return lerpf(a, b, tz);
}

/* Three bounded octaves of vector noise, keyed by (seed, species, axis path,
 * octave) and position, normalised to unit amplitude. */
static vec3 noise_vec(const Ctx *c, uint32_t axis, vec3 p) {
    vec3 s = v3(0, 0, 0);
    float amp = 1.0f, freq = 2.2f / maxf(c->k, 0.3f), norm = 0.0f;
    for (int o = 0; o < 3; ++o) {
        uint32_t h = hash_combine(hash_combine(c->key, axis), (uint32_t)o);
        vec3 q = v3_scale(p, freq);
        s = madd(s, v3(vnoise3(h, q) - 0.5f, vnoise3(h ^ 0x51u, q) - 0.5f, vnoise3(h ^ 0xA7u, q) - 0.5f), 2.0f * amp);
        norm += amp;
        amp *= 0.5f; freq *= 2.0f;
    }
    return v3_scale(s, 1.0f / norm);
}

/* ------------------------------------------------------------ emitters --- */

static void mF(MList *o, float len, float r0, float r1, int mat, float flex, float noise, float trop, uint32_t path, uint32_t axis) {
    Mod *m = put(o, T_F);
    m->k = (uint8_t)mat; m->path = path; m->axis = axis;
    m->a[0] = len; m->a[1] = r0; m->a[2] = r1; m->a[3] = flex; m->a[4] = noise; m->a[5] = trop;
}
static void mf(MList *o, float len) { put(o, T_f)->a[0] = len; }
static void mRot(MList *o, int s, float ang) { put(o, s)->a[0] = ang; }
static void mPush(MList *o) { put(o, T_PUSH); }
static void mPop(MList *o) { put(o, T_POP); }
static void mLeaf(MList *o, int shape, float len, float wid, float pet, float droop, float twist, uint32_t path, int prio) {
    Mod *m = put(o, T_L);
    m->k = (uint8_t)shape; m->path = path; m->i = (uint16_t)prio;
    m->a[0] = len; m->a[1] = wid; m->a[2] = pet; m->a[3] = droop; m->a[4] = twist;
}
static void mCluster(MList *o, int shape, float len, float ratio, int count, uint32_t path) {
    Mod *m = put(o, T_CLUSTER);
    m->k = (uint8_t)shape; m->path = path; m->i = PRIO_FINE;
    m->a[0] = len; m->a[1] = ratio; m->a[2] = (float)count;
}
static void mRoot(MList *o, float len, float curve, float r, uint32_t path) {
    Mod *m = put(o, T_W);
    m->path = path;
    m->a[0] = len; m->a[1] = curve; m->a[2] = r;
}
static Mod *mN(MList *o, int s, int i, uint32_t path) {
    Mod *m = put(o, s);
    m->i = (uint16_t)i; m->path = path;
    return m;
}

/* ===========================================================================
 * Species definitions (section 7)
 * ========================================================================= */

/* ---- 7.1 Kentia palm ---------------------------------------------------- */

/* How spread and old frond i is: 1 the oldest, outermost and most horizontal, 0 the spear. */
static float kentia_pf(const Ctx *c, int i) {
    float base = c->Nf > 1 ? 1.0f - (float)i / (float)(c->Nf - 1) : 0.5f;
    return clampf(0.85f * base + jit(c, node(TAG_C, i), CH_OPEN, 0.18f) + 0.08f, 0.0f, 1.0f);
}

static void setup_kentia(Ctx *c, MList *ax) {
    float H = c->H;
    c->Nt = (int)clampf(roundf(H / 0.12f), 10.0f, 22.0f);
    c->Nf = (int)clampf(roundf(8.0f + 2.0f * c->age), 7.0f, 12.0f);
    c->trunkLen = 0.48f * H;         /* the arching crown brings the plant to its height */
    mRot(ax, T_ROLL, rnd(c, TAG_K, CH_AZ) * 2.0f * PI_F);
    mRot(ax, T_PITCH, lerp_r(c, TAG_K, CH_LEAN, 1.0f, 4.0f) * DEG);
    mN(ax, N_TRUNK, 0, TAG_K);
    mN(ax, N_CROWN, 0, TAG_C);
}

static void prod_kentia(Ctx *c, const Mod *m, MList *o) {
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

static void setup_dracaena(Ctx *c, MList *ax) {
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

static void prod_dracaena(Ctx *c, const Mod *m, MList *o) {
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

static void setup_lyrata(Ctx *c, MList *ax) {
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

static void prod_lyrata(Ctx *c, const Mod *m, MList *o) {
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

static void setup_benjamina(Ctx *c, MList *ax) {
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

static void prod_benjamina(Ctx *c, const Mod *m, MList *o) {
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

static void setup_schefflera(Ctx *c, MList *ax) {
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

static void prod_schefflera(Ctx *c, const Mod *m, MList *o) {
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

static void setup_zz(Ctx *c, MList *ax) {
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

static void prod_zz(Ctx *c, const Mod *m, MList *o) {
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
static int expand(Ctx *c) {
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

static int add_seg(Ctx *c, vec3 a, vec3 b, float r0, float r1, int mat, int prev, int depth, int flags, float attachR, uint32_t path) {
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

static void interpret(Ctx *c) {
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

static void colonize(Ctx *c) {
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

/* ===========================================================================
 * Organ geometry (stage 6)
 * ========================================================================= */

typedef struct Shape {
    int   n;
    float t[10], w[10];
    float flex, edgeFlex, camber, wave, waveFreq;
    int   along[PQ_COUNT], across[PQ_COUNT];
} Shape;

/* Width profiles (fraction of length -> fraction of the widest point) and the
 * sag coefficients of section 2.4. camber > 0 cups the blade up along its
 * midrib; < 0 makes it convex. */
static const Shape SHAPES[LS_COUNT] = {
    /* kentia leaflet: long, narrow, folded along the midrib */
    { 5, { 0, 0.1f, 0.35f, 0.7f, 1 }, { 0.30f, 0.85f, 1, 0.7f, 0 }, 0.095f, 0.008f, 0.30f, 0.0f, 0.0f, { 2, 2, 3 }, { 2, 2, 2 } },
    /* dracaena: long, narrow, pointed */
    { 6, { 0, 0.06f, 0.25f, 0.6f, 0.85f, 1 }, { 0.55f, 0.9f, 1, 0.85f, 0.45f, 0 }, 0.070f, 0.008f, 0.14f, 0.0f, 0.0f, { 3, 5, 7 }, { 2, 2, 2 } },
    /* fiddle: constricted waist, broad upper lobe, rounded tip */
    { 10, { 0, 0.06f, 0.2f, 0.4f, 0.52f, 0.68f, 0.82f, 0.92f, 0.98f, 1 }, { 0.12f, 0.45f, 0.66f, 0.6f, 0.66f, 0.95f, 1.0f, 0.78f, 0.38f, 0 },
      0.115f, 0.022f, 0.08f, 0.03f, 5.0f, { 4, 6, 8 }, { 2, 4, 4 } },
    /* small elliptic: oval to lanceolate, pointed tip */
    { 7, { 0, 0.1f, 0.35f, 0.55f, 0.8f, 0.94f, 1 }, { 0.12f, 0.6f, 0.97f, 1, 0.68f, 0.22f, 0 }, 0.135f, 0.012f, 0.10f, 0.0f, 0.0f, { 2, 2, 3 }, { 1, 1, 2 } },
    /* schefflera leaflet: obovate-elliptic */
    { 7, { 0, 0.12f, 0.4f, 0.62f, 0.85f, 0.97f, 1 }, { 0.12f, 0.45f, 0.88f, 1, 0.82f, 0.35f, 0 }, 0.095f, 0.010f, 0.10f, 0.0f, 0.0f, { 2, 2, 3 }, { 2, 2, 2 } },
    /* zz: thick, elliptic, rounded tip, convex */
    { 7, { 0, 0.1f, 0.3f, 0.55f, 0.8f, 0.95f, 1 }, { 0.2f, 0.65f, 0.95f, 1, 0.82f, 0.42f, 0 }, 0.055f, 0.008f, -0.12f, 0.0f, 0.0f, { 2, 3, 4 }, { 2, 2, 2 } },
};

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

static void emit_organs(Ctx *c) {
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

static int sides_for(const Ctx *c, float r) {
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

static void emit_tubes(Ctx *c) {
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

/* ===========================================================================
 * Budgets (stage 8): estimate before building, prune supplements first
 * ========================================================================= */

static int est_leaf(const Ctx *c, int shape) {
    const Shape *s = &SHAPES[shape];
    int na = s->along[c->q] - c->detail, nc = s->across[c->q];
    if (na < 1) na = 1;
    if (nc > 2 && c->detail > 0) nc = 2;
    return na * nc * 2;
}

/* A conservative count of the triangles the organs and stems will make. */
static int estimate(const Ctx *c) {
    static const int PET[PQ_COUNT] = { 2, 3, 4 };
    int tris = 0, pet = PET[c->q] * 2 * 3;
    for (int i = 0; i < c->nseg; ++i) tris += 3 * sides_for(c, c->seg[i].r0);   /* the tube, and a tip cap */
    for (int i = 0; i < c->norg; ++i) {
        const Organ *o = &c->org[i];
        switch (o->type) {
        case O_LEAF:     tris += est_leaf(c, o->shape) + (o->a[2] > 0.012f ? pet : 0); break;
        case O_FROND:    tris += (int)(2.0f * o->a[2] * c->leafletKeep + 1.0f) * est_leaf(c, LS_KENTIA) + 16 * sides_for(c, 0.01f); break;
        case O_UMBRELLA: tris += (int)o->a[0] * est_leaf(c, LS_UMBRELLA) + pet; break;
        case O_CLUSTER:  tris += (int)o->a[2] * est_leaf(c, o->shape); break;
        default:         tris += 24; break;
        }
    }
    return tris;
}

static void budget(Ctx *c) {
    static const int TRI_CAP[PQ_COUNT] = { 5000, 14000, 28000 };
    int cap = TRI_CAP[c->q];
    if (c->P.maxTris > 0 && c->P.maxTris < cap) cap = c->P.maxTris;
    if (estimate(c) <= cap) return;
    /* 1. drop the supplementary colonised twigs and their foliage */
    int n = 0;
    for (int i = 0; i < c->norg; ++i) if (c->org[i].prio != PRIO_SUPPLEMENT) c->org[n++] = c->org[i]; else c->out->st.pruned++;
    c->norg = n;
    int ns = 0;
    for (int i = 0; i < c->nseg; ++i) if (!(c->seg[i].flags & SF_COLONY)) c->seg[ns++] = c->seg[i];   /* colony segments come last */
    c->nseg = ns;
    if (estimate(c) <= cap) return;
    /* 2. fewer leaflets in the compound leaves; 3. simpler blades and stems */
    c->leafletKeep = 0.7f;
    if (estimate(c) <= cap) return;
    c->detail = 1;
    if (estimate(c) <= cap) return;
    c->leafletKeep = 0.5f;
    c->detail = 2;
}

/* ===========================================================================
 * Crown occlusion (a coarse leaf-area grid) and validation
 * ========================================================================= */

#define OG 12

static void occlusion(Ctx *c) {
    PlantMesh *m = c->out;
    if (!m->count) return;
    float mn[3] = { 1e9f, 1e9f, 1e9f }, mx[3] = { -1e9f, -1e9f, -1e9f };
    for (uint32_t i = 0; i < m->count; ++i)
        for (int a = 0; a < 3; ++a) { mn[a] = minf(mn[a], m->v[i].p[a]); mx[a] = maxf(mx[a], m->v[i].p[a]); }
    float cs[3], *grid = (float *)calloc(OG * OG * OG, sizeof(float));
    if (!grid) oom();
    for (int a = 0; a < 3; ++a) { cs[a] = maxf((mx[a] - mn[a]) / (float)OG, 0.02f); }
    /* leaf area per cell */
    for (uint32_t i = 0; i + 2 < m->count; i += 3) {
        const PlantVertex *v = &m->v[i];
        if (v->part != PP_LEAF) continue;
        vec3 a = v3(v[0].p[0], v[0].p[1], v[0].p[2]), b = v3(v[1].p[0], v[1].p[1], v[1].p[2]), d = v3(v[2].p[0], v[2].p[1], v[2].p[2]);
        float area = 0.5f * v3_len(v3_cross(v3_sub(b, a), v3_sub(d, a)));
        vec3 ce = v3_scale(v3_add(v3_add(a, b), d), 1.0f / 3.0f);
        int ix = (int)clampf((ce.x - mn[0]) / cs[0], 0, OG - 1), iy = (int)clampf((ce.y - mn[1]) / cs[1], 0, OG - 1), iz = (int)clampf((ce.z - mn[2]) / cs[2], 0, OG - 1);
        grid[(iz * OG + iy) * OG + ix] += area;
    }
    /* optical depth up, up-and-out and out: leaf area met per unit cross-section */
    for (uint32_t i = 0; i < m->count; ++i) {
        PlantVertex *v = &m->v[i];
        vec3 p = v3(v->p[0], v->p[1], v->p[2]);
        vec3 out = v3(p.x, 0.0f, p.z);
        out = v3_len(out) > 1e-3f ? v3_norm(out) : v3(1, 0, 0);
        vec3 dirs[3] = { UP, v3_norm(v3_add(UP, out)), out };
        float vis = 0.0f;
        for (int k = 0; k < 3; ++k) {
            float tau = 0.0f;
            for (int s = 1; s <= 4; ++s) {
                vec3 q = madd(p, dirs[k], (float)s * 0.75f * (cs[0] + cs[1] + cs[2]) / 3.0f);
                int ix = (int)floorf((q.x - mn[0]) / cs[0]), iy = (int)floorf((q.y - mn[1]) / cs[1]), iz = (int)floorf((q.z - mn[2]) / cs[2]);
                if (ix < 0 || iy < 0 || iz < 0 || ix >= OG || iy >= OG || iz >= OG) break;
                tau += grid[(iz * OG + iy) * OG + ix] / (cs[0] * cs[2]);
            }
            vis += expf(-0.55f * tau);
        }
        v->ao = (uint8_t)clampf(255.0f * (0.25f + 0.75f * vis / 3.0f), 0.0f, 255.0f);
    }
    free(grid);
}

static int validate(const Ctx *c) {
    const PlantMesh *m = c->out;
    if (!m->count || m->count % 3) return 0;
    for (uint32_t i = 0; i < m->count; ++i) {
        const PlantVertex *v = &m->v[i];
        if (!isfinite(v->p[0]) || !isfinite(v->p[1]) || !isfinite(v->p[2]) || !isfinite(v->n[0])) return 0;
        if (v->p[1] < -c->P.floorDepth - 0.05f) return 0;
    }
    if (m->mx[1] > 1.5f * c->H + 0.1f) return 0;
    if (m->mx[0] - m->mn[0] > 3.5f * c->H || m->mx[2] - m->mn[2] > 3.5f * c->H) return 0;
    return m->st.leaves + m->st.leaflets > 0;
}

/* ===========================================================================
 * Entry point
 * ========================================================================= */

int plant_build(const PlantParams *pin, PlantMesh *out) {
    memset(out, 0, sizeof(*out));
    Ctx *c = (Ctx *)calloc(1, sizeof(Ctx));
    if (!c) oom();
    c->P = *pin;
    c->sp = pin->species < 0 || pin->species >= PS_COUNT ? 0 : pin->species;
    c->q = pin->quality < 0 ? 0 : pin->quality >= PQ_COUNT ? PQ_COUNT - 1 : pin->quality;
    c->key = hash_combine(hash_combine(pin->seed, 0x504C4E54u), (uint32_t)c->sp + 1u);
    c->H = clampf(pin->height, 0.3f, 4.0f);
    c->k = c->H / plant_info(c->sp)->refHeight;
    c->W = clampf(pin->widthScale, 0.3f, 2.0f);
    c->age = clampf(pin->age, 0.0f, 1.0f);
    c->G = pin->physics ? clampf(pin->gravity, 0.0f, 1.5f) : 0.0f;
    c->lv = clampf(pin->leafVariation, 0.0f, 0.4f);
    c->bv = clampf(pin->bendVariation, 0.0f, 0.3f);
    c->branchCap = 64;
    c->leafletKeep = 1.0f;
    c->out = out;

    switch (c->sp) {
    case PS_KENTIA:     setup_kentia(c, &c->cur); break;
    case PS_DRACAENA:   setup_dracaena(c, &c->cur); break;
    case PS_LYRATA:     setup_lyrata(c, &c->cur); break;
    case PS_BENJAMINA:  setup_benjamina(c, &c->cur); break;
    case PS_SCHEFFLERA: setup_schefflera(c, &c->cur); break;
    default:            setup_zz(c, &c->cur); break;
    }
    int ok = expand(c);
    out->st.passes = c->passes;
    out->st.modules = c->cur.n;
    if (ok) {
        interpret(c);
        colonize(c);
        budget(c);
        emit_organs(c);
        emit_tubes(c);
        out->st.branches = c->branches;
        for (int a = 0; a < 3; ++a) { out->mn[a] = 1e9f; out->mx[a] = -1e9f; }
        for (uint32_t i = 0; i < out->count; ++i)
            for (int a = 0; a < 3; ++a) { out->mn[a] = minf(out->mn[a], out->v[i].p[a]); out->mx[a] = maxf(out->mx[a], out->v[i].p[a]); }
        occlusion(c);
        ok = validate(c);
    }
    if (!ok) {
        free(out->v);
        PlantStats st = out->st;
        memset(out, 0, sizeof(*out));
        out->st = st;
    }
    free(c->cur.m); free(c->nxt.m); free(c->seg); free(c->org);
    free(c);
    return ok;
}

void plant_mesh_free(PlantMesh *m) { free(m->v); memset(m, 0, sizeof(*m)); }
