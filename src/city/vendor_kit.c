#include "city_internal.h"
#include "pmodel.h"

/* ===========================================================================
 * The near vendor layer (roadmap NB2, NB4 in ~/.claude/plans/street-vendors.md):
 * within KIT_DISTANCE, each canopy noodle bar's body (src/gen/noodle_bar.c:
 * its plan's structure as merged MAT_KIT triangles, baked occlusion and wear
 * per vertex; its rivets, bolts, stools, bell and A-frame as the modelling
 * kit's modules, Mesh.kit, drawn instanced from the module library by
 * src/render/kit_gpu.c). While this layer is resident the base layer's far
 * shell of each bar (mesh_vendor.c) gives way to it (city.vert).
 *
 * Each bar's cooking line (NB5, nb_line) is laid out from its plan: its
 * merged parts (the range's carcass, the bain-marie, the gas line, the
 * cables) are the body's, its modules set down with the rest. city_set_kit
 * picks who gets them: the bars (KIT_SLICE, the default), every vendor
 * (KIT_ALL: NB2's cost spike, a lane's worth of slices; a vendor that isn't a
 * bar gets the line's modules of the bar its site would hold), or none
 * (KIT_OFF: no vendor layer at all, the far shells everywhere).
 * ========================================================================= */

static int g_kit = KIT_SLICE;
void city_set_kit(int mode) { g_kit = mode; }
int  city_kit_mode(void) { return g_kit; }

typedef struct KitEmit { Mesh *m; PmInstances inst; PmMesh body, none, occ; } KitEmit;

/* A bar's flags but the heat: the vendor's wear (0..15) and its finish (its coat, its hood's metal, whether its
 * A-frame is bare timber: nb_finish; the caller keeps that last bit for the A-frame alone), and its tubes'
 * white (NB6). */
uint32_t vendor_kit_flags(const Vendor *v) {
    const uint32_t wear = (uint32_t)(clampf(v->wear, 0.0f, 1.0f) * 15.0f + 0.5f);
    return wear | nb_finish(v->site.affluence, v->site.seed) | (uint32_t)v->nb.tubeWhite << KIT_FLAG_GLOW_SHIFT;
}
/* A neon icon's glow bits (its colour and flicker), icon i of the plan. */
static uint32_t icon_glow(const NbPlan *p, int i) {
    return i >= 0 && i < p->nicon ? (uint32_t)p->iconGlow[i] << KIT_FLAG_GLOW_SHIFT : 0u;
}

/* The near body's triangles into the chunk, each split (into four, up to BODY_REFINE times) where the
 * cooking's grime changes across it: the grime rides the vertices, and a splashback or a hood's panel is a
 * few large triangles whose corners may all lie outside the grease or the steam (NB7). */
#define BODY_REFINE 3
typedef struct BodyEmit {
    Mesh *m; const VendorSpot *s; const Vendor *v; const NbLine *L; const NbSteam *st; int nst;
    uint32_t seed, wear; int far;
} BodyEmit;

static void body_vertex(const BodyEmit *B, const PmVertex *x, float g) {
    const VendorSpot *s = B->s;
    const float bx = (float)(s->x - B->m->ox), bz = (float)(s->z - B->m->oz);
    vec3 p = v3(bx + x->p[0] * s->fz + x->p[2] * s->fx, s->y + x->p[1], bz - x->p[0] * s->fx + x->p[2] * s->fz);
    vec3 n = v3(x->n[0] * s->fz + x->n[2] * s->fx, x->n[1], -x->n[0] * s->fx + x->n[2] * s->fz);
    uint8_t prm[4] = { MAT_KIT, x->mat, x->occ, x->wear };
    uint32_t fl = B->wear | (nb_at_range(&B->v->nb, x->p) ? KIT_FLAG_HEAT : 0u);
    if (x->mat == PMAT_SIGN_FAR)   /* a far icon face: its own neon's colour (its part: 140 + the icon) */
        fl = (fl & ((1u << KIT_FLAG_GLOW_SHIFT) - 1u)) | icon_glow(&B->v->nb, (int)x->part - 140);
    uint32_t aux = (uint32_t)x->part | (fl & 255u) << 8 | (B->far ? KIT_AUX_FAR : 0u) | (fl >> 8) << 17;
    mesh_push_w(B->m, p, n, (int8_t)lrintf(127.0f * g), x->uv[0], x->uv[1], B->seed, prm, aux);
}

static PmVertex body_mid(const PmVertex *a, const PmVertex *b) {
    PmVertex c = *a;
    for (int k = 0; k < 3; ++k) c.p[k] = 0.5f * (a->p[k] + b->p[k]);
    float nn = 0.0f;
    for (int k = 0; k < 3; ++k) { c.n[k] = a->n[k] + b->n[k]; nn += c.n[k] * c.n[k]; }
    nn = nn > 1e-12f ? 1.0f / sqrtf(nn) : 0.0f;
    for (int k = 0; k < 3; ++k) c.n[k] *= nn;
    c.uv[0] = 0.5f * (a->uv[0] + b->uv[0]); c.uv[1] = 0.5f * (a->uv[1] + b->uv[1]);
    c.occ = (uint8_t)((a->occ + b->occ + 1) / 2); c.wear = (uint8_t)((a->wear + b->wear + 1) / 2);
    return c;
}

static void body_tri(const BodyEmit *B, const PmVertex t[3], const float g[3], int depth) {
    if (depth < BODY_REFINE) {
        /* split when an edge is long and the grime at its middle isn't what the corners would give it */
        float e = 0.0f;
        for (int k = 0; k < 3; ++k) {
            const PmVertex *a = &t[k], *b = &t[(k + 1) % 3];
            const float d[3] = { a->p[0] - b->p[0], a->p[1] - b->p[1], a->p[2] - b->p[2] };
            e = maxf(e, d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        }
        if (e > 0.12f * 0.12f) {
            PmVertex m[3];
            float gm[3], miss = 0.0f;
            for (int k = 0; k < 3; ++k) {
                m[k] = body_mid(&t[k], &t[(k + 1) % 3]);
                gm[k] = nb_grime(&B->v->nb, B->L, B->st, B->nst, m[k].p, m[k].n);
                miss = maxf(miss, fabsf(gm[k] - 0.5f * (g[k] + g[(k + 1) % 3])));
            }
            if (miss > 0.06f) {
                const PmVertex a[3] = { t[0], m[0], m[2] }, b[3] = { m[0], t[1], m[1] }, c[3] = { m[2], m[1], t[2] }, d[3] = { m[0], m[1], m[2] };
                const float ga[3] = { g[0], gm[0], gm[2] }, gb[3] = { gm[0], g[1], gm[1] }, gc[3] = { gm[2], gm[1], g[2] }, gd[3] = { gm[0], gm[1], gm[2] };
                body_tri(B, a, ga, depth + 1); body_tri(B, b, gb, depth + 1);
                body_tri(B, c, gc, depth + 1); body_tri(B, d, gd, depth + 1);
                return;
            }
        }
    }
    for (int k = 0; k < 3; ++k) body_vertex(B, &t[k], g[k]);
}

void vendor_emit_body(Mesh *m, const VendorSpot *s, const Vendor *v, const PmMesh *pm, int far) {
    const uint32_t wear = vendor_kit_flags(v) & ~KIT_FLAG_BARE_WOOD;
    const uint32_t seed = hash_combine(s->site.seed, 0x4B42u);
    /* the near body's grime from the cooking (NB7): grease round the burners, condensation where the steam runs;
     * in the normal's w (shaders/city.vert: vLitter.z) */
    NbLine L;
    NbSteam st[NB_STEAMS_MAX];
    int nst = 0;
    if (!far) { nb_line(&v->nb, &L); nst = nb_steam(&v->nb, &L, s->site.ceiling, st, NB_STEAMS_MAX); }
    m->grounded = 1;
    m->ground = s->y;   /* its heights count from its floor */
    BodyEmit B = { m, s, v, &L, st, nst, seed, wear, far };
    for (uint32_t i = 0; i + 2 < pm->ni; i += 3) {
        PmVertex t[3];
        float g[3];
        for (int k = 0; k < 3; ++k) {
            t[k] = pm->v[pm->idx[i + k]];
            g[k] = far ? 0.0f : nb_grime(&v->nb, &L, st, nst, t[k].p, t[k].n);
        }
        body_tri(&B, t, g, far ? BODY_REFINE : 0);
    }
    m->grounded = 0;
}

static void kit_vendor(void *ctx, const VendorSpot *s, const Vendor *v) {
    KitEmit *E = (KitEmit *)ctx;
    if (g_kit == KIT_OFF) return;
    const int bar = v->archetype == VA_CANOPY;
    const int line = bar || g_kit == KIT_ALL;
    if (!line) return;
    Mesh *m = E->m;
    NbPlan other;
    const NbPlan *np = &v->nb;
    if (!bar) { nb_plan(&other, s->site.w, s->site.d, s->site.ceiling, s->site.seed, -1, v->trade, s->site.affluence); np = &other; }
    E->inst.n = 0;
    Pm pm;
    if (bar) {
        E->body.nv = E->body.ni = 0;
        pm_begin(&pm, &E->body, &E->inst, PM_LOD0);
        nb_body(&pm, np);
        nb_line_occluders(np, &E->occ);   /* the line darkens the bench and counter round it (NB6) */
        pm_bake_with(&E->body, &E->occ, NB_BAKE_RAYS, pm_reach(&E->body));
        vendor_emit_body(m, s, v, &E->body, 0);
    }
    NbLine L;
    nb_line(np, &L);
    pm_begin(&pm, &E->none, &E->inst, PM_LOD0);
    nb_line_place(&pm, np, &L);
    NbSteam st[NB_STEAMS_MAX];
    const int nst = nb_steam(np, &L, s->site.ceiling, st, NB_STEAMS_MAX);
    /* the site's frame to chunk-local: x across (fz, -fx), z out (fx, fz), y up from its floor */
    const float R[3][3] = { { s->fz, 0.0f, s->fx }, { 0.0f, 1.0f, 0.0f }, { -s->fx, 0.0f, s->fz } };
    const float t[3] = { (float)(s->x - m->ox), s->y, (float)(s->z - m->oz) };
    /* the flags: the vendor's wear and finish, and whether a module stands at the range (its grease and soot) */
    const uint32_t wear = vendor_kit_flags(v);
    if (m->nkit + E->inst.n > m->capkit) {
        m->capkit = (m->nkit + E->inst.n) * 2;
        m->kit = (KitInst *)realloc(m->kit, sizeof(KitInst) * m->capkit);
        if (!m->kit) fatal("out of memory setting down the vendors' kit");
    }
    for (uint32_t i = 0; i < E->inst.n; ++i) {
        const PmInstance *pi = &E->inst.v[i];
        KitInst *k = &m->kit[m->nkit++];
        memset(k, 0, sizeof(*k));   /* its padding too: chunks compare and hash kit records byte for byte */
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 4; ++c)
                k->xf[r * 4 + c] = R[r][0] * pi->xf[c] + R[r][1] * pi->xf[4 + c] + R[r][2] * pi->xf[8 + c];
            k->xf[r * 4 + 3] += t[r];
        }
        k->module = pi->module;
        k->part = pi->part;
        const float at[3] = { pi->xf[3], pi->xf[7], pi->xf[11] };
        const int heat = nb_at_range(np, at);
        uint32_t fin = pi->module == PMK_A_FRAME ? wear : wear & ~KIT_FLAG_BARE_WOOD;
        k->seed = hash_combine(s->site.seed, i + 0x4B49u);
        /* the A-frame's chalk menu reads the trade from its seed's top byte (kit.glsl kitChalk, SF1) */
        if (pi->module == PMK_A_FRAME) k->seed = (k->seed & 0x00FFFFFFu) | (uint32_t)np->trade << 24;
        if (pm_module_is_icon(pi->module)) {
            /* a neon icon (its part 140 + its place on the fascia): its colour and flicker, its light's seed */
            const int ic = (int)pi->part - 140;
            fin = (fin & ((1u << KIT_FLAG_GLOW_SHIFT) - 1u)) | icon_glow(np, ic);
            if (ic >= 0 && ic < np->nicon) k->seed = nb_icon_seed(np, ic);
        }
        k->flags = (uint16_t)(fin | (heat ? KIT_FLAG_HEAT : 0u));
        k->grime = (int8_t)lrintf(127.0f * nb_grime(np, &L, st, nst, at, NULL));
        k->base = s->y;
        for (int a = 0; a < 3; ++a) {
            float p = k->xf[a * 4 + 3];
            m->mn[a] = minf(m->mn[a], p - KIT_PAD);
            m->mx[a] = maxf(m->mx[a], p + KIT_PAD);
        }
    }
}

void city_build_vendor_kit(int cx, int cz, Mesh *m) {
    mesh_begin(m, cx, cz);
    m->nkit = 0;
    if (g_kit == KIT_OFF) return;
    KitEmit E;
    memset(&E, 0, sizeof(E));
    E.m = m;
    city_chunk_vendors(cx, cz, kit_vendor, &E);
    pm_inst_free(&E.inst);
    pm_mesh_free(&E.body);
    pm_mesh_free(&E.none);
    pm_mesh_free(&E.occ);
}
