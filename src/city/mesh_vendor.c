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
 * Every vendor is built on the kit from its plan (src/gen/noodle_bar.c): its
 * far shell goes in the chunk's base layer (seen to the view distance,
 * sheltering the rain, casting nothing: its body casts, and city.vert drops the
 * shell where that is drawn), its body is streamed on its own near the camera
 * (vendor_kit.c), its lights go in the chunk's light table and its steam in
 * its plume list. The lane vaults and the arcology halls' battens are here too.
 * (SV2's shells, MAT_VENDOR, and SV3's painted rooms were retired in SF6.)
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

/* What blocks a walker: a vendor's plan's solids (the kitchen behind its counter, posts, column, stools, A-frame),
 * each with what shows it (the hours: the A-frame and stools leave while shut). */
int vendor_solids(const Vendor *v, VendorSolid *out, int max) {
    float b[NB_SOLIDS_MAX][4];
    uint8_t show[NB_SOLIDS_MAX];
    const uint8_t hours = vendor_hours(v);
    int n = 0, nb = nb_solids_shown(&v->nb, b, show, NB_SOLIDS_MAX);
    for (int i = 0; i < nb && n < max; ++i) {
        VendorSolid *o = &out[n++];
        o->x0 = b[i][0]; o->z0 = b[i][1]; o->x1 = b[i][2]; o->z1 = b[i][3]; o->y = 0.0f; o->floor = 0;
        o->show = show[i]; o->hours = hours;
    }
    return n;
}

/* ------------------------------------------------------------- the shell --- */

typedef struct VEmit { Mesh *m; ChunkGen *g; PmMesh shell; int tris, nbody; } VEmit;

/* A noodle bar's steam and smoke (NB7: nb_steam) as the chunk's plumes, for the smoke pass (chunks_plumes). */
static void vendor_plumes(Mesh *m, const VendorSpot *s, const Vendor *v) {
    NbLine L;
    nb_line(&v->nb, &L);
    NbSteam st[NB_STEAMS_MAX];
    const int n = nb_steam(&v->nb, &L, s->site.ceiling, st, NB_STEAMS_MAX);
    const float bx = (float)(s->x - m->ox), bz = (float)(s->z - m->oz);
    for (int i = 0; i < n; ++i) {
        const NbSteam *e = &st[i];
        vec3 c = v3(bx + e->pos[0] * s->fz + e->pos[2] * s->fx, s->y + e->pos[1], bz - e->pos[0] * s->fx + e->pos[2] * s->fz);
        push_plume(m, c, e->radius, e->rise, hash_combine(s->site.seed, 0x57EAu + (uint32_t)i),
                   e->kind == NB_STEAM_WOK ? PLUME_WOK : PLUME_STALL);
        Plume *p = &m->plume[m->nplume - 1];
        p->strength = e->strength;
        p->dir[0] = s->fx; p->dir[1] = s->fz;   /* the site's +z, out to the street */
        p->nknot = (uint32_t)e->nknot;
        p->hours = vendor_hours(v);   /* SF4: none while shut (steam.vert) */
        for (int k = 0; k < e->nknot && k < PLUME_KNOTS; ++k) memcpy(p->knot[k], e->knot[k], sizeof(e->knot[k]));
        for (int k = 0; k < 3; ++k) memcpy(p->light[k], e->light[k], sizeof(e->light[k]));
    }
}

static void emit_vendor(void *ctx, const VendorSpot *s, const Vendor *v) {
    VEmit *E = (VEmit *)ctx;
    Mesh *m = E->m;
    /* its far shell (src/gen/noodle_bar.c), which gives way to its body near (vendor_kit.c) */
    E->shell.nv = E->shell.ni = 0;
    Pm pm;
    pm_begin(&pm, &E->shell, NULL, PM_LOD2);
    nb_shell(&pm, &v->nb);
    pm_bake(&E->shell, NB_BAKE_RAYS, pm_reach(&E->shell));
    nb_shell_shutters(&pm, &v->nb);   /* shown while shut at any distance (city.vert) */
    /* SF2: its body is streamed on its own near the camera (Mesh.vbody, the streamer's): its far shell gives
     * way while that's resident */
    const int idx = E->nbody++;
    if (idx < VENDOR_BODIES_MAX) {
        if (m->nvbody == m->capvbody) {
            m->capvbody = m->capvbody ? m->capvbody * 2 : 16;
            m->vbody = (VendorBodySpot *)realloc(m->vbody, sizeof(VendorBodySpot) * m->capvbody);
            if (!m->vbody) fatal("out of memory listing the vendors' bodies");
        }
        double mx, mz;
        vendor_spot_middle(s, &mx, &mz);
        VendorBodySpot *b = &m->vbody[m->nvbody++];
        b->x = (float)(mx - m->ox); b->z = (float)(mz - m->oz); b->y = s->y;
        b->r = 0.5f * sqrtf(s->site.w * s->site.w + s->site.d * s->site.d);
        b->hours = vendor_hours(v);
    }
    vendor_emit_body(m, s, v, &E->shell, idx < VENDOR_BODIES_MAX ? 1 : 2, 0, idx);
    E->tris += (int)(E->shell.ni / 3);
    vendor_lights(E->g, m, s, v);
    vendor_plumes(m, s, v);
}

/* SF2: the bays of the lane vaults (src/gen/vault.c) whose middles the chunk holds, in its base layer (seen far,
 * sheltering the rain, casting), and their pendant lamps. */
static void build_vaults(Mesh *m, ChunkGen *g, PmMesh *pmm) {
    const double P = CHUNK_SIZE;
    CityVault *cv = (CityVault *)malloc(sizeof(CityVault) * 32);
    if (!cv) fatal("out of memory building the vaults");
    const int n = city_vendor_vaults(g, g->ox, g->oz, g->ox + P, g->oz + P, cv, 32);
    for (int i = 0; i < n; ++i) {
        const CityVault *V = &cv[i];
        VendorSpot fr;
        memset(&fr, 0, sizeof(fr));
        fr.x = V->x; fr.z = V->z; fr.fx = V->fx; fr.fz = V->fz; fr.y = V->y;
        /* as a vendor's: its wear by the district, its paint and its sheet's metal (nb_finish) */
        const float wearF = clampf(1.0f - V->affluence + 0.3f * (hash_float(hash_combine(V->seed, 9u)) - 0.5f), 0.0f, 1.0f);
        const uint32_t flags = (uint32_t)(wearF * 15.0f + 0.5f) | (nb_finish(V->affluence, V->seed) & ~NB_FINISH_BARE_WOOD);
        NbLight L[16];
        int nl = 0;
        for (int b = 0; b < V->plan.nbay; ++b) {
            float x0, x1;
            vault_bay_range(&V->plan, b, &x0, &x1);
            double wx, wz;
            vault_world(V, 0.5 * (x0 + x1), 0.5 * V->plan.span, &wx, &wz);
            if (wx < g->ox || wx >= g->ox + P || wz < g->oz || wz >= g->oz + P) continue;
            pmm->nv = pmm->ni = 0;
            Pm pm;
            pm_begin(&pm, pmm, NULL, PM_LOD1);
            pm.part = 32;
            vault_bay(&pm, &V->plan, b);
            pm_bake(pmm, 6, 0.6f);
            vendor_emit_kit(m, &fr, pmm, flags, hash_combine(V->seed, (uint32_t)b));
            float at[3];
            if (nl < 16 && vault_lamp(&V->plan, b, at)) {
                NbLight *l = &L[nl++];
                memset(l, 0, sizeof(*l));
                /* a short tube under the shade: a tube's reach (a lamp's falls short of the floor from 4 m) */
                l->kind = NB_LIGHT_TUBE; l->colour = NB_WHITE_5000;
                l->a[0] = at[0] - 0.15f; l->a[1] = at[1]; l->a[2] = at[2];
                l->b[0] = at[0] + 0.15f; l->b[1] = at[1]; l->b[2] = at[2];
                l->seed = (uint8_t)hash_combine(V->seed, 0x1A0u + (uint32_t)b);
            }
        }
        if (nl) vendor_light_list(g, m, &fr, L, nl, 0);   /* the lane's: always lit */
    }
    free(cv);
}

/* SF5b: the food halls' battens' light (their geometry's the vendor layer's, vendor_kit.c kit_hall): each a tube for
 * each of its two lamps, the first always lit, the second keeping the hours of the bar nearest it; those whose middles
 * the chunk holds. */
typedef struct HallBody { uint64_t key; VendorBodySpot spot; } HallBody;
typedef struct HallLights { Mesh *m; ChunkGen *g; HallBody body[32]; int nbody; } HallLights;
static void light_hall(void *ctx, const Building *b, const ALayout *A, float floorY, int hall) {
    HallLights *E = (HallLights *)ctx;
    const AHall *H = &A->fhall[hall];
    /* its furniture's body in this chunk (city_hall_in_chunk), listed after the bars' */
    double mx, mz;
    float r;
    if (E->nbody < 32 && city_hall_in_chunk(b, A, hall, E->g->ox, E->g->oz, &mx, &mz, &r)) {
        HallBody *o = &E->body[E->nbody++];
        o->key = city_hall_key(b, hall);
        o->spot.x = (float)(mx - E->m->ox); o->spot.z = (float)(mz - E->m->oz); o->spot.y = floorY;
        o->spot.r = r + HALL_BODY_REACH;
        o->spot.hours = 0;
    }
    const uint8_t white = H->use == AB_NOODLE ? NB_WHITE_6500 : NB_WHITE_5000;
    for (int k = 0; k < H->nbatten; ++k) {
        const float *bt = H->batten[k];
        VendorSpot fr;
        memset(&fr, 0, sizeof(fr));
        city_arc_world(b, A, bt[0], bt[1], &fr.x, &fr.z);
        if (fr.x < E->g->ox || fr.x >= E->g->ox + CHUNK_SIZE || fr.z < E->g->oz || fr.z >= E->g->oz + CHUNK_SIZE) continue;
        float ux, uz;   /* along it, world: the frame's +x as the site's (its +x is (fz, -fx)) */
        city_arc_dir(b, A, bt[2] > 0.5f ? 0.0f : 1.0f, bt[2] > 0.5f ? 1.0f : 0.0f, &ux, &uz);
        fr.fz = ux; fr.fx = -uz;
        fr.y = floorY;
        /* one light for its two tubes: 2.88 m long (a tube's strength goes by its length: both tubes' worth) over its
         * 1.5 m; every other batten's on the nearest bar's hours, the rest always lit, so a hall whose bars are all
         * shut keeps half its light (the user: dimmed) and the frame pays for one light a batten, not two */
        NbLight l;
        memset(&l, 0, sizeof(l));
        l.kind = NB_LIGHT_TUBE; l.colour = white;
        const float y = arcology_hall_batten_y(H->roof);
        l.a[0] = -1.44f; l.a[1] = y; l.a[2] = 0.0f;
        l.b[0] = 1.44f;  l.b[1] = y; l.b[2] = 0.0f;
        l.seed = (uint8_t)hash_combine(H->seed, 0xBA70u + (uint32_t)k);
        l.flicker = 1;   /* a hall's batten: its reach short (shaders/common.glsl vendorReach) */
        vendor_light_list(E->g, E->m, &fr, &l, 1, (k & 1) ? city_hall_batten_hours(b, A, hall, k) : 0);
    }
}

void build_vendors(Mesh *m, ChunkGen *g) {
    VEmit E;
    memset(&E, 0, sizeof(E));
    E.m = m;
    E.g = g;
    build_vaults(m, g, &E.shell);
    HallLights *HL = (HallLights *)calloc(1, sizeof(HallLights));
    if (!HL) fatal("out of memory building the halls");
    HL->m = m; HL->g = g;
    city_halls(g, 110.0, light_hall, HL);
    city_vendors(g, emit_vendor, &E);
    /* SF5b: the halls' furniture as bodies after the bars', in their keys' order */
    for (int i = 0; i < HL->nbody; ++i)
        for (int j = i + 1; j < HL->nbody; ++j)
            if (HL->body[j].key < HL->body[i].key) { HallBody t = HL->body[i]; HL->body[i] = HL->body[j]; HL->body[j] = t; }
    for (int i = 0; i < HL->nbody && E.nbody < VENDOR_BODIES_MAX; ++i, ++E.nbody) {
        if (m->nvbody == m->capvbody) {
            m->capvbody = m->capvbody ? m->capvbody * 2 : 16;
            m->vbody = (VendorBodySpot *)realloc(m->vbody, sizeof(VendorBodySpot) * m->capvbody);
            if (!m->vbody) fatal("out of memory listing the vendors' bodies");
        }
        m->vbody[m->nvbody++] = HL->body[i].spot;
    }
    free(HL);
    pm_mesh_free(&E.shell);
}
