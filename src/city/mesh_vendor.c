#include "city_internal.h"

/* ===========================================================================
 * Street vendors in the city (roadmap SV2 in ~/.claude/plans/street-vendors.md,
 * after docs/street-vendor-generation.md section 5).
 *
 * Each site (vendor_sites.c) gets the grammar's vendor (src/gen/vendor.c) at
 * the standard tier. A vendor sees the fingerprints of the vendors before it in
 * its group (the doc's section 8), so a chunk generates the earlier members of
 * any group it owns a member of, wherever their sites lie: the group is a
 * block's, planned whole whichever chunk asks, so every chunk sees the same
 * vendors.
 *
 * Its shell (vendor_mesh's VL_SHELL: the structure, its signs and neon) goes in
 * the chunk's base layer: seen to the view distance, drawn into the rain's
 * shelter map, casting ray-traced shadows. A canopy noodle bar (NB4) is made
 * from its plan instead (src/gen/noodle_bar.c): its far shell here (sheltering
 * the rain, casting nothing: its body in the vendor layer casts, and city.vert
 * drops the shell where that is drawn), its body in vendor_kit.c. Its dressing waits for a near-only
 * layer of its own (SV5). Lit neon faces are drawn as the facades' signs
 * (MAT_NEON); everything else is MAT_VENDOR (src/core/common.h), shaded by
 * shaders/city/vendor.glsl. The rooms behind its openings are painted by
 * shaders/city/vendor_room.glsl (SV3) from the words room_word puts on their quads.
 * ========================================================================= */

#define VENDORS_ALL 1024   /* every site of the blocks reaching a chunk */

int city_vendors(ChunkGen *g, VendorVisit fn, void *ctx) {
    const double P = CHUNK_SIZE;
    VendorSpot *sp = (VendorSpot *)malloc(sizeof(VendorSpot) * VENDORS_ALL);
    VendorFinger *fin = (VendorFinger *)malloc(sizeof(VendorFinger) * VENDORS_ALL);
    VendorFinger *cx = (VendorFinger *)malloc(sizeof(VendorFinger) * VENDORS_ALL);
    uint8_t *need = (uint8_t *)malloc(VENDORS_ALL);
    uint32_t *later = (uint32_t *)malloc(sizeof(uint32_t) * VENDORS_ALL);
    Vendor *v = (Vendor *)malloc(sizeof(Vendor));
    if (!sp || !fin || !cx || !need || !later || !v) fatal("out of memory generating vendors");
    int n = city_vendor_spots(g, g->ox, g->oz, g->ox + P, g->oz + P, VSPOTS_BLOCKS, sp, VENDORS_ALL);
    /* which to generate: the chunk's own, and every member before them in their groups (members come in order) */
    int nlater = 0;
    for (int i = n - 1; i >= 0; --i) {
        double mx, mz;
        vendor_spot_middle(&sp[i], &mx, &mz);
        int own = mx >= g->ox && mx < g->ox + P && mz >= g->oz && mz < g->oz + P;
        int ahead = 0;
        for (int k = 0; k < nlater && !ahead; ++k) ahead = later[k] == sp[i].group;
        need[i] = (uint8_t)(own ? 2 : ahead ? 1 : 0);
        if (own && !ahead) later[nlater++] = sp[i].group;
    }
    int made = 0;
    for (int i = 0; i < n; ++i) {
        if (!need[i]) continue;
        int nc = 0;
        for (int j = 0; j < i; ++j)
            if (need[j] && sp[j].group == sp[i].group) cx[nc++] = fin[j];
        int ok = vendor_generate(&sp[i].site, VTIER_STANDARD, cx, nc, v, NULL);
        fin[i] = v->finger;
        if (need[i] == 2 && ok) {
            if (fn) fn(ctx, &sp[i], v);
            made++;
        }
    }
    free(v); free(later); free(need); free(cx); free(fin); free(sp);
    return made;
}

int city_chunk_vendors(int cx, int cz, VendorVisit fn, void *ctx) {
    ChunkGen g;
    chunk_gen_init(&g, cx, cz, PROPS_PLAN_PAD, PROPS_BUILD_PAD, PROPS_LAMP_PAD);
    int n = city_vendors(&g, fn, ctx);
    chunk_gen_free(&g);
    return n;
}

/* ------------------------------------------------------------ collision --- */

/* What blocks a walker: the vendor's body behind the front its customers stand
 * at (its containers, if it has them, whose decks are walked on), and whatever
 * solid stands outside that at body height (posts, cloths to the ground, a
 * pair's second container, a market's table). Thin parts are grown to 12 cm. */
int vendor_solids(const Vendor *v, VendorSolid *out, int max) {
    int n = 0;
    if (v->archetype == VA_CANOPY) {
        /* a noodle bar: its plan's (the kitchen behind its counter, posts, column, stools, A-frame) */
        float b[NB_SOLIDS_MAX][4];
        int nb = nb_solids(&v->nb, b, NB_SOLIDS_MAX);
        for (int i = 0; i < nb && n < max; ++i) {
            VendorSolid *o = &out[n++];
            o->x0 = b[i][0]; o->z0 = b[i][1]; o->x1 = b[i][2]; o->z1 = b[i][3]; o->y = 0.0f; o->floor = 0;
        }
        return n;
    }
#define ADD(a, b, c, d, yy, fl) do { if (n < max) { VendorSolid *o_ = &out[n++]; o_->x0 = (a); o_->z0 = (b); o_->x1 = (c); o_->z1 = (d); o_->y = (yy); o_->floor = (uint8_t)(fl); } } while (0)
    float body[2][4];
    int nb = 0;
    if (v->ncbox)
        for (int k = 0; k < v->ncbox; ++k) { body[nb][0] = v->cbox[k][0]; body[nb][1] = v->cbox[k][1]; body[nb][2] = v->cbox[k][2]; body[nb][3] = v->cbox[k][3]; nb++; }
    else if (v->front > maxf(v->box[1], 0.0f) + 0.05f) {
        body[0][0] = v->box[0]; body[0][1] = maxf(v->box[1], 0.0f); body[0][2] = v->box[2]; body[0][3] = v->front;
        nb = 1;
    }
    for (int k = 0; k < nb; ++k) ADD(body[k][0], body[k][1], body[k][2], body[k][3], 0.0f, 0);
    for (int i = 0; i < v->nstructure; ++i) {
        const VPart *p = &v->part[i];
        float mn[3], mx[3];
        vendor_part_aabb(p, mn, mx);
        if (p->role == VR_DECK || p->role == VR_STEP) { ADD(mn[0], mn[2], mx[0], mx[2], mx[1], 1); continue; }
        int solid;
        switch (p->role) {
        case VR_SHELL: case VR_SUPPORT: case VR_COUNTER: case VR_TABLE: case VR_TIER: case VR_BED: case VR_CABINET:
        case VR_WHEEL: case VR_FLUE: case VR_GAS:
            solid = mn[1] < 1.8f && mx[1] > 0.15f; break;
        case VR_CLOTH:
            solid = mn[1] < 0.5f && mx[1] > 0.15f; break;
        default:
            solid = 0; break;
        }
        if (!solid) continue;
        int inside = 0;
        for (int k = 0; k < nb && !inside; ++k)
            inside = mn[0] >= body[k][0] - 0.05f && mx[0] <= body[k][2] + 0.05f && mn[2] >= body[k][1] - 0.05f && mx[2] <= body[k][3] + 0.05f;
        if (inside) continue;
        float cxm = 0.5f * (mn[0] + mx[0]), czm = 0.5f * (mn[2] + mx[2]);
        float hx = maxf(0.5f * (mx[0] - mn[0]), 0.06f), hz = maxf(0.5f * (mx[2] - mn[2]), 0.06f);
        ADD(cxm - hx, czm - hz, cxm + hx, czm + hz, 0.0f, 0);
    }
#undef ADD
    return n;
}

/* ------------------------------------------------------------- the shell --- */

static uint32_t to_srgb8(const float lin[3]) {
    uint32_t o = 0;
    for (int k = 0; k < 3; ++k) {
        float l = clampf(lin[k], 0.0f, 1.0f);
        float s = l <= 0.0031308f ? l * 12.92f : 1.055f * powf(l, 1.0f / 2.4f) - 0.055f;
        o |= (uint32_t)(s * 255.0f + 0.5f) << (16 - 8 * k);
    }
    return o;
}

typedef struct VEmit { Mesh *m; VendorMesh vm; PmMesh shell; int tris; } VEmit;

/* The opening a room quad's vertex lies on (in its plane, inside it): its index, or -1. */
static int room_opening(const Vendor *v, const float p[3]) {
    for (int i = 0; i < v->nopen; ++i) {
        const VOpening *o = &v->open[i];
        float d[3] = { p[0] - o->o[0], p[1] - o->o[1], p[2] - o->o[2] };
        float along = d[0] * o->u[0] + d[2] * o->u[2], out = d[0] * o->n[0] + d[2] * o->n[2];
        if (fabsf(out) < 0.01f && along > -0.01f && along < o->w + 0.01f && d[1] > -0.01f && d[1] < o->h + 0.01f) return i;
    }
    return -1;
}

static uint8_t q_cm25(float m) { return (uint8_t)clampf(m / 0.025f + 0.5f, 1.0f, 255.0f); }

/* What the shader needs to paint the room behind an opening (shaders/city/vendor_room.glsl vendorRoom; the
 * layout is in src/core/common.h): its size in prm z, w; its depth in the normal's w; in aux its
 * stock, feature wall, kind, palette, cooking, wear, lamp and whether it is shut; in the seed the
 * lamp's colour and a hash of its own. */
static void room_word(const Vendor *v, int oi, uint8_t prm[4], uint32_t *aux, uint32_t *seed, int8_t *depth) {
    const VOpening *o = &v->open[oi];
    prm[2] = q_cm25(o->w);
    prm[3] = q_cm25(o->h);
    *depth = (int8_t)clampf(o->depth / 0.025f + 0.5f, 1.0f, 127.0f);
    uint32_t wear = (uint32_t)clampf(v->wear * 7.0f + 0.5f, 0.0f, 7.0f);
    int tube = v->nlight > 0 && (v->light[0].kind == VLK_TUBE || v->light[0].kind == VLK_LED);
    int closed = 0;   /* SV8 keeps hours */
    *aux = (uint32_t)(v->stock[0] & 31u) | (uint32_t)(v->stock[1] & 31u) << 5 | (uint32_t)(v->stock[2] & 31u) << 10 |
           (uint32_t)(v->feature & 7u) << 15 | (uint32_t)(o->kind & 7u) << 18 | (uint32_t)(v->palette & 15u) << 21 |
           (uint32_t)(v->cooking & 3u) << 25 | wear << 27 | (uint32_t)tube << 30 | (uint32_t)closed << 31;
    uint32_t lamp = v->nlight > 0 ? v->light[0].rgb : v->part[o->part].rgb;
    uint32_t r = (lamp >> 16) & 255u, g = (lamp >> 8) & 255u, b = lamp & 255u;
    uint32_t c565 = (r >> 3) << 11 | (g >> 2) << 5 | (b >> 3);
    *seed = (hash_combine(v->site.seed, (uint32_t)oi + 0x7700u) & 0xFFFFu) | c565 << 16;
}

static void emit_vendor(void *ctx, const VendorSpot *s, const Vendor *v) {
    VEmit *E = (VEmit *)ctx;
    Mesh *m = E->m;
    if (v->archetype == VA_CANOPY) {
        /* a noodle bar: its far shell (src/gen/noodle_bar.c), which gives way to its body near (vendor_kit.c) */
        E->shell.nv = E->shell.ni = 0;
        Pm pm;
        pm_begin(&pm, &E->shell, NULL, PM_LOD2);
        nb_shell(&pm, &v->nb);
        pm_bake(&E->shell, NB_BAKE_RAYS, pm_reach(&E->shell));
        vendor_emit_body(m, s, v, &E->shell, 1);
        E->tris += (int)(E->shell.ni / 3);
        return;
    }
    VendorMesh *vm = &E->vm;
    vm->count = 0;
    vendor_mesh(v, VL_SHELL, vm);
    if (!vm->count) return;
    uint32_t pal[4];
    for (int k = 0; k < 4; ++k) {
        float c[3];
        vendor_palette_rgb(v->palette, k, c);
        pal[k] = to_srgb8(c);
    }
    float bx = (float)(s->x - m->ox), bz = (float)(s->z - m->oz);
    uint8_t wear = (uint8_t)(clampf(v->wear, 0.0f, 1.0f) * 255.0f + 0.5f);
    m->grounded = 1;
    m->ground = s->y;   /* its heights count from its floor */
    for (uint32_t i = 0; i + 2 < vm->count; i += 3) {
        const VendorVertex *t = &vm->v[i];
        uint8_t prm[4] = { MAT_VENDOR, t->surf, t->flags, wear };
        uint32_t aux = (t->col == VCOL_RGB ? t->rgb & 0xFFFFFFu : t->col < 4 ? pal[t->col] : 0x808080u) | (uint32_t)t->role << 24;
        uint32_t seed = v->site.seed;
        float uvs[3][2];
        int8_t wt = -1;   /* the normal's w: the part's top, or a room's depth */
        for (int k = 0; k < 3; ++k) { uvs[k][0] = t[k].uv[0]; uvs[k][1] = t[k].uv[1]; }
        if (t->surf == VS_ROOM) {
            int oi = room_opening(v, t->p);
            if (oi >= 0) room_word(v, oi, prm, &aux, &seed, &wt);
        }
        if (t->surf == VS_NEON) {
            /* a sign's face: a quad's two triangles, which share their first corner (vendor_mesh.c quad); uv over
             * it 0..1, its size in half metres; lit tubes as the facades' neon */
            uint32_t q = i >= 3 && vm->v[i - 3].surf == VS_NEON && !memcmp(vm->v[i - 3].p, t->p, sizeof(t->p)) ? i - 3 : i;
            float uw = 0.01f, vh = 0.01f;
            for (uint32_t k = q; k < q + 6 && k < vm->count; ++k) { uw = maxf(uw, vm->v[k].uv[0]); vh = maxf(vh, vm->v[k].uv[1]); }
            for (int k = 0; k < 3; ++k) { uvs[k][0] /= uw; uvs[k][1] /= vh; }
            uint32_t word = t->rgb;
            uint8_t wM = (uint8_t)clampf(uw * 2.0f + 0.5f, 1.0f, 255.0f), hM = (uint8_t)clampf(vh * 2.0f + 0.5f, 1.0f, 255.0f);
            seed = hash_combine(v->site.seed, q);
            if ((t->flags & VPF_EMISSIVE) && !(word & (1u << 30))) {
                /* colour, a border round boards (not round a motif's frame), the flicker */
                uint32_t style = (word >> 24) & 1u ? 0u : 1u;
                prm[0] = MAT_NEON;
                prm[1] = (uint8_t)((word & 7u) | style << 3 | ((word >> 16) & 3u) << 6);
            }
            prm[2] = wM; prm[3] = hM;
            aux = word;
        }
        for (int k = 0; k < 3; ++k) {
            const VendorVertex *x = &t[k];
            vec3 p = v3(bx + x->p[0] * s->fz + x->p[2] * s->fx, s->y + x->p[1], bz - x->p[0] * s->fx + x->p[2] * s->fz);
            vec3 nn = v3(x->n[0] * s->fz + x->n[2] * s->fx, x->n[1], -x->n[0] * s->fx + x->n[2] * s->fz);
            /* its part's top over the floor, 5 cm steps (to 6.35 m), where the grime runs down from (vendor.glsl) */
            mesh_push_w(m, p, nn, wt >= 0 ? wt : (int8_t)(x->top < 127 ? x->top : 127), uvs[k][0], uvs[k][1], seed, prm, aux);
        }
        E->tris++;
    }
    m->grounded = 0;
}

void build_vendors(Mesh *m, ChunkGen *g) {
    VEmit E;
    memset(&E, 0, sizeof(E));
    E.m = m;
    city_vendors(g, emit_vendor, &E);
    vendor_mesh_free(&E.vm);
    pm_mesh_free(&E.shell);
}
