#include "plant_internal.h"

/* ===========================================================================
 * Lobby plants (LPLS-1.19). See src/gen/plant.h for the outline. The stages run
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

vec3 madd(vec3 a, vec3 b, float s) { return v3(a.x + b.x * s, a.y + b.y * s, a.z + b.z * s); }
/* Rodrigues: v turned by angle a about unit axis k. */
vec3 rot_axis(vec3 v, vec3 k, float a) {
    float c = cosf(a), s = sinf(a);
    return v3_add(v3_add(v3_scale(v, c), v3_scale(v3_cross(k, v), s)), v3_scale(k, v3_dot(k, v) * (1.0f - c)));
}
vec3 perp_of(vec3 d) {
    vec3 a = fabsf(d.y) < 0.9f ? UP : v3(1.0f, 0.0f, 0.0f);
    return v3_norm(v3_cross(d, a));
}
int finite3(vec3 v) { return isfinite(v.x) && isfinite(v.y) && isfinite(v.z); }

/* ------------------------------------------------------------- modules --- */

void oom(void) { fprintf(stderr, "FATAL: out of memory growing a plant\n"); exit(1); }

Mod *put(MList *l, int s) {
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

/* ---------------------------------------------------------- randomness --- */

uint32_t child(uint32_t path, uint32_t tag, int i) { return hash_combine(hash_combine(path, tag), (uint32_t)i); }
uint32_t node(uint32_t axis, int i) { return hash_combine(axis ^ 0x9E3779B9u, (uint32_t)i); }
/* rand(node, channel) in [0, 1) */
float rnd(const Ctx *c, uint32_t path, uint32_t ch) { return hash_float(hash_combine(hash_combine(c->key, path), ch)); }
/* jitter(node, channel, amplitude) in [-amp, amp] */
float jit(const Ctx *c, uint32_t path, uint32_t ch, float amp) { return (rnd(c, path, ch) * 2.0f - 1.0f) * amp; }
float lerp_r(const Ctx *c, uint32_t path, uint32_t ch, float lo, float hi) { return lo + (hi - lo) * rnd(c, path, ch); }

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
vec3 noise_vec(const Ctx *c, uint32_t axis, vec3 p) {
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

void mF(MList *o, float len, float r0, float r1, int mat, float flex, float noise, float trop, uint32_t path, uint32_t axis) {
    Mod *m = put(o, T_F);
    m->k = (uint8_t)mat; m->path = path; m->axis = axis;
    m->a[0] = len; m->a[1] = r0; m->a[2] = r1; m->a[3] = flex; m->a[4] = noise; m->a[5] = trop;
}
void mf(MList *o, float len) { put(o, T_f)->a[0] = len; }
void mRot(MList *o, int s, float ang) { put(o, s)->a[0] = ang; }
void mPush(MList *o) { put(o, T_PUSH); }
void mPop(MList *o) { put(o, T_POP); }
void mLeaf(MList *o, int shape, float len, float wid, float pet, float droop, float twist, uint32_t path, int prio) {
    Mod *m = put(o, T_L);
    m->k = (uint8_t)shape; m->path = path; m->i = (uint16_t)prio;
    m->a[0] = len; m->a[1] = wid; m->a[2] = pet; m->a[3] = droop; m->a[4] = twist;
}
void mCluster(MList *o, int shape, float len, float ratio, int count, uint32_t path) {
    Mod *m = put(o, T_CLUSTER);
    m->k = (uint8_t)shape; m->path = path; m->i = PRIO_FINE;
    m->a[0] = len; m->a[1] = ratio; m->a[2] = (float)count;
}
void mRoot(MList *o, float len, float curve, float r, uint32_t path) {
    Mod *m = put(o, T_W);
    m->path = path;
    m->a[0] = len; m->a[1] = curve; m->a[2] = r;
}
Mod *mN(MList *o, int s, int i, uint32_t path) {
    Mod *m = put(o, s);
    m->i = (uint16_t)i; m->path = path;
    return m;
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
