#include "vendor_internal.h"
#include <stdarg.h>
#include <string.h>

/* ===========================================================================
 * Street vendors: the stages (section 2 of docs/street-vendor-generation.md)
 * and the retry ladder (section 12). See vendor.h.
 *
 * Every random decision comes from a stream keyed by the site's seed, a name
 * and (for the structure, dressing and fill) the level and attempt, so a seed
 * always gives the same vendor and a reroll of the dressing doesn't move the
 * structure.
 * ========================================================================= */

void vlog(VGen *G, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    blog_vprintf(G->log, fmt, ap);
    va_end(ap);
}

/* ------------------------------------------------------------ the parts --- */

int v_part(Vendor *v, int shape, int role, int surf, int col, int layer) {
    if (v->npart >= V_MAX_PART) return -1;
    VPart *p = &v->part[v->npart];
    memset(p, 0, sizeof(*p));
    p->shape = (uint8_t)shape; p->role = (uint8_t)role; p->surf = (uint8_t)surf; p->col = (uint8_t)col; p->layer = (uint8_t)layer;
    p->segs = 8;
    p->seed = hash_combine(v->site.seed ^ 0x9A27u, (uint32_t)v->npart);
    return v->npart++;
}

int v_box(Vendor *v, int role, int surf, int col, float x0, float y0, float z0, float x1, float y1, float z1) {
    int i = v_part(v, VP_BOX, role, surf, col, VL_SHELL);
    if (i < 0) return -1;
    VPart *p = &v->part[i];
    p->a[0] = minf(x0, x1); p->a[1] = minf(y0, y1); p->a[2] = minf(z0, z1);
    p->b[0] = maxf(x0, x1); p->b[1] = maxf(y0, y1); p->b[2] = maxf(z0, z1);
    return i;
}

int v_beam(Vendor *v, int role, int surf, int col, float ax, float ay, float az, float bx, float by, float bz, float size) {
    int i = v_part(v, VP_BEAM, role, surf, col, VL_SHELL);
    if (i < 0) return -1;
    VPart *p = &v->part[i];
    p->a[0] = ax; p->a[1] = ay; p->a[2] = az; p->b[0] = bx; p->b[1] = by; p->b[2] = bz; p->r = size;
    return i;
}

int v_cyl(Vendor *v, int role, int surf, int col, float ax, float ay, float az, float bx, float by, float bz, float r, int segs) {
    int i = v_part(v, VP_CYL, role, surf, col, VL_SHELL);
    if (i < 0) return -1;
    VPart *p = &v->part[i];
    p->a[0] = ax; p->a[1] = ay; p->a[2] = az; p->b[0] = bx; p->b[1] = by; p->b[2] = bz; p->r = r; p->segs = (uint8_t)segs;
    return i;
}

int v_quad(Vendor *v, int role, int surf, int col, const float q[4][3], int flags) {
    int i = v_part(v, VP_QUAD, role, surf, col, VL_SHELL);
    if (i < 0) return -1;
    VPart *p = &v->part[i];
    memcpy(p->q, q, sizeof(p->q));
    p->flags = (uint8_t)flags;
    p->segs = 6;
    for (int k = 0; k < 3; ++k) {
        p->a[k] = minf(minf(q[0][k], q[1][k]), minf(q[2][k], q[3][k]));
        p->b[k] = maxf(maxf(q[0][k], q[1][k]), maxf(q[2][k], q[3][k]));
    }
    return i;
}

void v_edge(Vendor *v, int from, int to, int kind) {
    if (from < 0 || to < 0 || v->nedge >= V_MAX_EDGE) return;
    VEdge *e = &v->edge[v->nedge++];
    e->from = (uint16_t)from; e->to = (uint16_t)to; e->kind = (uint8_t)kind;
}

void v_slot(Vendor *v, int kind, float x, float y, float z, float nx, float nz, float w, float h) {
    if (v->nslot >= V_MAX_SLOT || w < 0.1f) return;
    VSlot *s = &v->slot[v->nslot++];
    memset(s, 0, sizeof(*s));
    s->kind = (uint8_t)kind; s->c[0] = x; s->c[1] = y; s->c[2] = z; s->n[0] = nx; s->n[2] = nz; s->w = w; s->h = h;
}

int v_opening(Vendor *v, int kind, float ox, float oy, float oz, float ux, float uz, float w, float h, float depth, int part) {
    if (v->nopen >= V_MAX_OPEN || w < 0.2f || h < 0.2f) return -1;
    VOpening *o = &v->open[v->nopen];
    memset(o, 0, sizeof(*o));
    o->o[0] = ox; o->o[1] = oy; o->o[2] = oz;
    o->u[0] = ux; o->u[2] = uz;
    o->n[0] = -uz; o->n[2] = ux;   /* u x up: out of the opening, towards whoever sees it */
    o->w = w; o->h = h; o->depth = depth; o->kind = (uint8_t)kind; o->cbox = 255;
    /* the quad the room is painted on, in the opening's plane */
    float q[4][3] = { { ox, oy, oz }, { ox + ux * w, oy, oz + uz * w }, { ox + ux * w, oy + h, oz + uz * w }, { ox, oy + h, oz } };
    int r = v_quad(v, VR_ROOM, VS_ROOM, VCOL_RGB, q, 0);
    if (r >= 0) {
        v->part[r].rgb = V_TRADES[v->trade].lamp;
        v_edge(v, r, part, VE_OPEN_INTO);
    }
    o->part = (uint16_t)(r >= 0 ? r : 0);
    return v->nopen++;
}

int v_light(Vendor *v, int kind, float x, float y, float z, float power, uint32_t rgb) {
    if (v->nlight >= V_MAX_LIGHT) return -1;
    VLight *L = &v->light[v->nlight];
    memset(L, 0, sizeof(*L));
    L->kind = (uint8_t)kind; L->p[0] = x; L->p[1] = y; L->p[2] = z; L->power = power; L->rgb = rgb;
    return v->nlight++;
}

int v_sign(Vendor *v, int kind, float cx, float cy, float cz, float nx, float nz, float w, float h, int neon) {
    if (v->nsign >= V_MAX_SIGN) return -1;
    VSign *s = &v->sign[v->nsign];
    memset(s, 0, sizeof(*s));
    s->kind = (uint8_t)kind; s->neon = (uint8_t)neon;
    s->c[0] = cx; s->c[1] = cy; s->c[2] = cz; s->n[0] = nx; s->n[2] = nz; s->w = w; s->h = h;
    s->glyphs = hash_combine(v->site.seed ^ 0x5161u, (uint32_t)v->nsign);
    s->color = V_PALETTE[v->palette].neon[v->nsign & 1];
    return v->nsign++;
}

VItem *v_item(Vendor *v, int kind, int role) {
    if (v->nitem >= V_MAX_ITEM) return NULL;
    VItem *it = &v->item[v->nitem++];
    memset(it, 0, sizeof(*it));
    it->kind = (uint8_t)kind; it->role = (uint8_t)role; it->fz = 1.0f;
    return it;
}

int v_finish_surf(const Vendor *v) {
    static const uint8_t S[VF_COUNT] = { VS_STEEL, VS_CORRUGATED, VS_TILE, VS_TIMBER, VS_CHIPBOARD, VS_RUST };
    return S[v->finish < VF_COUNT ? v->finish : 0];
}

static void remove_item(Vendor *v, int i) {
    if (i < 0 || i >= v->nitem) return;
    memmove(&v->item[i], &v->item[i + 1], sizeof(VItem) * (size_t)(v->nitem - i - 1));
    v->nitem--;
}

/* ------------------------------------------------- archetype and trade --- */

/* A class's weight blended by affluence: poor at 0, middling at 0.5, wealthy at 1. */
static float by_affluence(const float w[VC_COUNT], float a) {
    return a < 0.5f ? lerpf(w[0], w[1], a * 2.0f) : lerpf(w[1], w[2], (a - 0.5f) * 2.0f);
}

static int fits(const VendorSite *s, int a) {
    float w, d;
    vendor_min_site(a, &w, &d);
    if (s->w < w || s->d < d) return 0;
    if (a == VA_LEANTO && !s->hostWall) return 0;
    /* SF2: the arcade counter only under a ceiling (it hangs from it) unless forced; a canopy bar's hood and
     * duct want room over it (its plan says whether it fits) */
    if (a == VA_ARCADE) return s->ceiling > 0.0f ? s->ceiling >= 2.55f : nb_forced_form() == NB_ARCADE;
    if (a == VA_TIERED) return nb_forced_form() == NB_TIERED || s->archetype == VA_TIERED;
    if (s->ceiling > 0.0f && s->ceiling < (a == VA_BARROW ? 2.5f : a == VA_CONTAINER || a == VA_PAIR ? 3.0f : a == VA_CANOPY ? 3.1f : 2.9f)) return 0;
    return 1;
}

static float arch_weight(const VendorSite *s, int cls, int t, int a) {
    if (!fits(s, a)) return 0.0f;
    /* SF2: the arcade and tiered forms take the canopy bar's affinity for the trade */
    float w = V_TRADES[t].arch[a == VA_ARCADE || a == VA_TIERED ? VA_CANOPY : a] * V_ARCH_CLASS[cls][a] * V_ARCH_SITE[s->kind < VSITE_COUNT ? s->kind : 0][a];
    if (s->lunch) w *= V_ARCH_LUNCH[a];
    /* forced for captures (NeonCity --nbform): the form's archetype wherever it fits, else the site's own choice */
    const int f = nb_forced_form();
    if (f >= 0 && w > 0.0f && (f == NB_STRAIGHT || f == NB_CORNER ? a == VA_CANOPY : vendor_kit_form(a) == f)) w *= 1e4f;
    if (f == NB_TIERED && a == VA_TIERED) w = 1e4f;
    return w;
}

static int g_trade = -1;
void vendor_force_trade(int t) { g_trade = t >= 0 && t < VT_COUNT ? t : -1; }
int  vendor_forced_trade(void) { return g_trade; }
int  vendor_trade_by_name(const char *name) {
    static const char *const KEY[VT_COUNT] = { "noodles", "soup", "dumplings", "skewers", "kebab", "burgers", "fried",
                                               "curry", "tacos", "crepes", "roast", "drinks" };
    for (int t = 0; t < VT_COUNT; ++t) if (strcmp(name, KEY[t]) == 0) return t;
    return -1;
}

static int pick_trade(const VendorSite *s, int cls, Rng *r) {
    if (g_trade >= 0) return g_trade;
    if (s->trade >= 0 && s->trade < VT_COUNT) return s->trade;
    float w[VT_COUNT];
    for (int t = 0; t < VT_COUNT; ++t) {
        float aw = 0.0f;
        for (int a = 0; a < VA_COUNT; ++a) aw += arch_weight(s, cls, t, a);
        w[t] = aw > 0.0f ? by_affluence(V_TRADES[t].cls, s->affluence) * (s->lunch ? V_TRADES[t].lunch : 1.0f) : 0.0f;
        if (s->archetype >= 0) w[t] *= V_TRADES[t].arch[s->archetype];
    }
    int t = v_pick(r, w, VT_COUNT);
    return t < 0 ? VT_NOODLE : t;
}

/* --------------------------------------------------------------- dressing --- */

static uint32_t decor_mask(const Vendor *v) {
    uint32_t m = 0;
    for (int i = 0; i < v->nitem; ++i)
        if (v->item[i].role == VR_DECOR && v->item[i].kind >= VI_DECOR_FIRST) m |= 1u << (v->item[i].kind - VI_DECOR_FIRST);
    return m;
}

static void fingerprint(Vendor *v) {
    VendorFinger *f = &v->finger;
    f->trade = (uint8_t)v->trade; f->palette = v->palette; f->layout = (uint8_t)(v->archetype * 32 + (v->layout & 31));
    f->shelf = v->shelfMix; f->feature = v->feature; f->motif = v->motif; f->signature = v->signature; f->finish = v->finish;
    f->decor = decor_mask(v);
}

int vendor_finger_shared(const VendorFinger *a, const VendorFinger *b) {
    return (a->trade == b->trade) + (a->palette == b->palette) + (a->layout == b->layout) + (a->shelf == b->shelf) +
           (a->feature == b->feature) + (a->motif == b->motif) + (a->signature == b->signature && a->signature < VSIG_COUNT) + (a->finish == b->finish) +
           (a->decor == b->decor);
}

/* Whether an item w across and d deep, set in slot s, stays inside the site. */
static int slot_fits(const Vendor *v, const VSlot *s, float w, float d) {
    float out = s->kind == VSL_WALL ? d : 0.5f * d;
    float cx = s->c[0] + s->n[0] * out, cz = s->c[2] + s->n[2] * out;
    float ex = 0.5f * (fabsf(s->n[2]) * w + fabsf(s->n[0]) * (s->kind == VSL_WALL ? 0.0f : d));
    float ez = 0.5f * (fabsf(s->n[0]) * w + fabsf(s->n[2]) * (s->kind == VSL_WALL ? 0.0f : d));
    return cx - ex >= -0.5f * v->site.w + 0.02f && cx + ex <= 0.5f * v->site.w - 0.02f && cz - ez >= 0.02f && cz + ez <= v->site.d - 0.02f;
}

/* An unused slot of this kind at least w x h that keeps a d-deep item inside the site; its index or -1. */
static int find_slot(Vendor *v, int kind, float w, float h, float d, Rng *r) {
    int cand[V_MAX_SLOT], n = 0;
    for (int i = 0; i < v->nslot; ++i)
        if (!v->slot[i].used && v->slot[i].kind == kind && v->slot[i].w >= w && v->slot[i].h >= h && slot_fits(v, &v->slot[i], w, d)) cand[n++] = i;
    return n ? cand[rng_u32(r) % (uint32_t)n] : -1;
}

/* Puts an item of w x h x d in slot si, facing out of it: a wall item's back on the wall,
 * a ground item standing on the ground, a roof item on the roof. */
static VItem *slot_item(Vendor *v, int si, int kind, int role, float w, float h, float d) {
    VSlot *s = &v->slot[si];
    VItem *it = v_item(v, kind, role);
    if (!it) return NULL;
    s->used = 1;
    it->fx = s->n[0]; it->fz = s->n[2];
    it->s[0] = w; it->s[1] = h; it->s[2] = d;
    if (s->kind == VSL_WALL) {
        it->p[0] = s->c[0] + s->n[0] * 0.5f * d; it->p[1] = s->c[1] - 0.5f * h; it->p[2] = s->c[2] + s->n[2] * 0.5f * d;
    } else {
        it->p[0] = s->c[0]; it->p[1] = s->c[1]; it->p[2] = s->c[2];
    }
    return it;
}

static int place_decor(Vendor *v, VGen *G, int kind, Rng *r) {
    const VDecorInfo *d = &V_DECOR[kind - VI_DECOR_FIRST];
    int si = find_slot(v, d->slot, d->w, d->slot == VSL_ROOF ? 0.0f : d->h, d->d, r);
    if (si < 0) return 0;
    VItem *it = slot_item(v, si, kind, VR_DECOR, d->w, d->h, d->d);
    if (!it) return 0;
    it->seed = rng_u32(r);
    it->rgb = V_PALETTE[v->palette].c[2 + (it->seed & 1)];
    (void)G;
    return 1;
}

static int has_rail(const VFrame *F) { return F->nrail > 0; }
static int has_counter(const VFrame *F) {
    for (int i = 0; i < F->nsurf; ++i) if (F->surf[i].kind == VSU_COUNTER || F->surf[i].kind == VSU_SHELF || F->surf[i].kind == VSU_TABLE) return 1;
    return 0;
}

/* A signature's second place, where its first has no room: on a counter (3), on the ground. */
static const uint8_t SIG_ALT[VSIG_COUNT] = { 255, 3, 255, 255, 255, 255, 255, 3, 255, 255, VSL_GROUND, VSL_GROUND };
/* The narrowest a signature may be made to fit its place (pennants, planters, screens and murals shrink). */
static const float SIG_MINW[VSIG_COUNT] = { 0.18f, 0.4f, 0.5f, 1.8f, 0.6f, 1.0f, 0.0f, 0.5f, 1.0f, 0.35f, 0.45f, 0.8f };

static VItem *sig_at(Vendor *v, const VFrame *F, int sig, int slot, Rng *r) {
    const VSigInfo *S = &V_SIG[sig];
    VItem *it = NULL;
    if (slot == 3) {
        /* on a counter's end */
        for (int i = 0; i < F->nsurf && !it; ++i) {
            const VSurface *s = &F->surf[i];
            if (s->kind != VSU_COUNTER && s->kind != VSU_SHELF && s->kind != VSU_TABLE) continue;
            if (s->x1 - s->x0 < maxf(0.6f, S->w + 0.15f) || s->z1 - s->z0 < S->d) continue;
            it = v_item(v, sig, VR_SIGNATURE);
            if (!it) return NULL;
            int right = rng_float(r) < 0.5f;
            float hw = 0.5f * S->w + 0.03f;
            it->p[0] = right ? s->x1 - hw : s->x0 + hw; it->p[1] = s->y; it->p[2] = 0.5f * (s->z0 + s->z1);
        }
    } else if (slot == 4) {
        if (!F->nrail) return NULL;
        const VRail *R = &F->rail[rng_u32(r) % (uint32_t)F->nrail];
        float len = R->outside ? R->y - V_HANG_CLEAR - 0.02f : 1.0f;
        if (len < S->h) return NULL;
        it = v_item(v, sig, VR_SIGNATURE);
        if (!it) return NULL;
        float t = rng_range(r, 0.15f, 0.35f);
        it->p[0] = lerpf(R->x0, R->x1, t); it->p[1] = R->y; it->p[2] = lerpf(R->z0, R->z1, t);
    } else {
        int si = find_slot(v, slot, SIG_MINW[sig], slot == VSL_ROOF ? 0.0f : S->h, S->d, r);
        if (si < 0) return NULL;
        float w = minf(S->w, v->slot[si].w);
        it = slot_item(v, si, sig, VR_SIGNATURE, w, S->h, S->d);
    }
    return it;
}

static int sig_possible(const Vendor *v, const VFrame *F, int sig) {
    int slots[2] = { V_SIG[sig].slot, SIG_ALT[sig] };
    for (int k = 0; k < 2; ++k) {
        int sl = slots[k];
        if (sl == 255) { if (k == 0) return 1; continue; }
        if (sl == 3 && has_counter(F)) return 1;
        if (sl == 4 && has_rail(F)) return 1;
        if (sl <= VSL_ROOF)
            for (int i = 0; i < v->nslot; ++i) if (v->slot[i].kind == sl && !v->slot[i].used) return 1;
    }
    return 0;
}

static int place_signature(Vendor *v, const VFrame *F, int sig, Rng *r) {
    const VSigInfo *S = &V_SIG[sig];
    if (S->slot == 255) return 1;   /* the half-dead neon: a flicker, set in the fill */
    VItem *it = sig_at(v, F, sig, S->slot, r);
    if (!it && SIG_ALT[sig] != 255) it = sig_at(v, F, sig, SIG_ALT[sig], r);
    if (!it) return 0;
    it->sig = 1;
    if (it->s[0] <= 0.0f) it->s[0] = S->w;
    it->s[1] = S->h; it->s[2] = S->d;
    it->seed = rng_u32(r);
    it->rgb = V_PALETTE[v->palette].c[2];
    return 1;
}

/* Chooses palette, finish, feature wall, motif, shelf stock, decorations and the signature
 * (section 7, 8). Returns 0 if no signature could be placed that a neighbour hasn't got. */
static int dress(VGen *G, const VFrame *F, const VendorFinger *ctx, int n, Rng *r) {
    Vendor *v = G->v;
    const VTradeInfo *T = &V_TRADES[v->trade];
    float pw[V_PALETTES];
    for (int p = 0; p < V_PALETTES; ++p) pw[p] = (V_PALETTE[p].classes >> v->cls) & 1u ? 1.0f : 0.06f;
    v->palette = (uint8_t)v_pick(r, pw, V_PALETTES);
    float fw[VF_COUNT];
    memcpy(fw, V_FINISH_CLASS[v->cls], sizeof(fw));
    if (v->wear > 0.7f) fw[VF_RUST] += 1.0f;
    v->finish = (uint8_t)v_pick(r, fw, VF_COUNT);
    float ww[VW_COUNT] = { 3.0f, T->cooking ? 3.0f : 0.5f, 1.0f, v->cls == VC_POOR ? 1.5f : 0.5f,
                           v->trade == VT_DRINKS ? 1.5f : 0.4f,
                           0.3f,
                           v->archetype == VA_MARKET || v->trade == VT_ROAST ? 2.0f : 0.5f,
                           v->cls == VC_RICH ? 2.0f : 0.5f };
    v->feature = (uint8_t)v_pick(r, ww, VW_COUNT);
    float mw[VM_COUNT];
    for (int m = 0; m < VM_COUNT; ++m) mw[m] = 0.3f;
    for (int k = 0; k < 3; ++k) if (T->motifs[k] < VM_COUNT) mw[T->motifs[k]] += 3.0f - (float)k;
    v->motif = (uint8_t)v_pick(r, mw, VM_COUNT);
    int ns = 0;
    while (ns < 5 && T->stock[ns] != 255) ns++;
    for (int k = 0; k < 3; ++k) {
        float sw[5] = { 0 };
        for (int i = 0; i < ns; ++i) sw[i] = (float)(ns - i);
        int pick = ns ? v_pick(r, sw, ns) : -1;
        v->stock[k] = pick >= 0 ? T->stock[pick] : (uint8_t)(rng_u32(r) % VK_COUNT);
        if (rng_float(r) < 0.15f) v->stock[k] = (uint8_t)(rng_u32(r) % VK_COUNT);   /* a sideline */
    }
    v->shelfMix = (uint8_t)(hash_combine(hash_combine(v->stock[0], v->stock[1]), v->stock[2]) & 255u);
    /* decorations: more of them, and grubbier ones, the poorer the street */
    int most = V_TIER[v->tier].decor;
    int want = G->level >= 2 ? 0 : (int)floorf((float)most * (0.3f + 0.7f * G->clutter) * (G->level ? 0.5f : 1.0f) + rng_float(r));
    if (want > most) want = most;
    float dw[VI_DECOR_COUNT];
    for (int d = 0; d < VI_DECOR_COUNT; ++d) {
        const VDecorInfo *D = &V_DECOR[d];
        dw[d] = D->squalor <= G->clutter + 0.1f ? 1.0f : 0.0f;
        if (v->cls == VC_RICH && D->squalor >= 0.3f) dw[d] = 0.0f;
    }
    dw[VI_EXTRACTOR - VI_DECOR_FIRST] *= T->cooking ? 3.0f : 0.3f;
    dw[VI_DGAS - VI_DECOR_FIRST] *= T->cooking == 2 ? 0.0f : 0.5f;     /* cooks get their own bottles */
    dw[VI_PLANTER - VI_DECOR_FIRST] *= v->cls == VC_RICH ? 3.0f : 0.5f;
    dw[VI_MENUBOARD - VI_DECOR_FIRST] *= T->cooking || v->cls == VC_RICH ? 2.5f : 0.7f;
    dw[VI_CHAIR - VI_DECOR_FIRST] *= vendor_is_kit(v->archetype) ? 0.3f : 1.0f;
    for (int k = 0, tries = 0; k < want && tries < want * 4 + 4; ++tries) {
        int d = v_pick(r, dw, VI_DECOR_COUNT);
        if (d < 0) break;
        if (place_decor(v, G, VI_DECOR_FIRST + d, r)) { k++; dw[d] *= 0.25f; }
        else dw[d] = 0.0f;
    }
    /* the signature: never one a neighbour has */
    float sw[VSIG_COUNT];
    for (int s = 0; s < VSIG_COUNT; ++s) {
        const VSigInfo *S = &V_SIG[s];
        sw[s] = 1.0f;
        if (!sig_possible(v, F, s)) sw[s] = 0.0f;
        (void)S;
        if (s == VSIG_SHRINE && !T->cooking) sw[s] *= 0.4f;
        if (s == VSIG_FISHRACK && v->trade != VT_NOODLE && v->trade != VT_SOUP) sw[s] *= 0.2f;
        if (s == VSIG_DEADNEON) sw[s] = v->cls == VC_RICH ? 0.1f : 1.2f;
        for (int i = 0; i < n; ++i) if (ctx[i].signature == s) sw[s] = 0.0f;
    }
    for (int tries = 0; tries < VSIG_COUNT; ++tries) {
        int s = v_pick(r, sw, VSIG_COUNT);
        if (s < 0) break;
        if (place_signature(v, F, s, r)) { v->signature = (uint8_t)s; fingerprint(v); return 1; }
        sw[s] = 0.0f;
    }
    /* no room for one the neighbours haven't got: none, rather than a copy */
    v->signature = VSIG_COUNT;
    vlog(G, "  info: no signature piece fits that the neighbours lack\n");
    fingerprint(v);
    return 1;
}

static int too_alike(const Vendor *v, const VendorFinger *ctx, int n, int *worst) {
    int bad = 0;
    *worst = 0;
    for (int i = 0; i < n; ++i) {
        int s = vendor_finger_shared(&v->finger, &ctx[i]);
        if (s > *worst) *worst = s;
        if (s > V_FINGER_SHARE || (ctx[i].signature == v->finger.signature && v->finger.signature < VSIG_COUNT)) bad = 1;
    }
    return bad;
}

/* ------------------------------------------------------------------ fill --- */

typedef struct Foot { float x0, z0, x1, z1, y; } Foot;

static int overlaps(const Foot *f, int n, float x0, float z0, float x1, float z1, float y) {
    for (int i = 0; i < n; ++i)
        if (fabsf(f[i].y - y) < 0.3f && x0 < f[i].x1 && x1 > f[i].x0 && z0 < f[i].z1 && z1 > f[i].z0) return 1;
    return 0;
}

static float hang_len(int kind) {
    switch (kind) {
    case VI_LANTERN:  return 0.38f;
    case VI_BUNCH:    return 0.42f;
    case VI_DUCK:     return 0.5f;
    case VI_CABLE:    return 0.55f;
    case VI_GARMENT:  return 0.85f;
    case VI_BUNTING:  return 0.28f;
    case VI_TOOLS:    return 0.45f;
    case VI_JOINT:    return 0.55f;
    case VI_UMBRELLA: return 0.6f;
    default:          return 0.4f;
    }
}

static void fill(VGen *G, const VFrame *F, Rng *r) {
    Vendor *v = G->v;
    const VTradeInfo *T = &V_TRADES[v->trade];
    const VTierInfo *TI = &V_TIER[v->tier];
    Foot foot[V_MAX_ITEM];
    int nf = 0;
    for (int i = 0; i < v->nitem; ++i) {
        const VItem *it = &v->item[i];
        if (it->role != VR_SIGNATURE || V_SIG[it->kind].slot != 3) continue;
        foot[nf].x0 = it->p[0] - 0.1f; foot[nf].x1 = it->p[0] + 0.1f; foot[nf].z0 = it->p[2] - 0.1f; foot[nf].z1 = it->p[2] + 0.1f; foot[nf].y = it->p[1]; nf++;
    }
    /* counter props: the cooking ones at the cook, then the rest wherever they fit */
    int np = 0, want;
    int nprops = 0;
    while (nprops < 6 && T->props[nprops] != 255) nprops++;
    if (G->level == 0) want = (int)((float)TI->props * (0.45f + 0.35f * G->clutter)) + 2;
    else if (G->level == 1) want = (int)((float)TI->props * 0.35f) + 1;
    else want = 2;
    if (want > TI->props) want = TI->props;
    for (int k = 0, tries = 0; k < want && tries < want * 6 && nprops; ++tries) {
        int kind;
        if (k == 0 && G->level == 2) kind = VI_CASHBOX;
        else {
            float pw[6] = { 0 };
            for (int i = 0; i < nprops; ++i) pw[i] = (float)(nprops - i) * (V_PROP[T->props[i]].emitter && np >= 2 ? 0.3f : 1.0f);
            kind = T->props[v_pick(r, pw, nprops)];
        }
        const VPropInfo *P = &V_PROP[kind];
        float sc = rng_range(r, 0.9f, 1.1f), w = P->w * sc, d = P->d * sc, h = P->h * sc;
        int placed = 0;
        /* cooking things go where the cooking is */
        if (P->emitter && P->emitter != VEM_MIST + 1 && F->cook[1] >= 0.0f) {
            for (int s = 0; s < F->nsurf && !placed; ++s) {
                const VSurface *S = &F->surf[s];
                if (fabsf(S->y - F->cook[1]) > 0.05f || F->cook[0] < S->x0 - 0.3f || F->cook[0] > S->x1 + 0.3f || F->cook[2] < S->z0 - 0.3f || F->cook[2] > S->z1 + 0.3f) continue;
                for (int t = 0; t < 6 && !placed; ++t) {
                    float x = clampf(F->cook[0] + rng_range(r, -0.6f, 0.6f), S->x0 + 0.5f * w, S->x1 - 0.5f * w), z = clampf(F->cook[2], S->z0 + 0.5f * d, S->z1 - 0.5f * d);
                    if (S->x1 - S->x0 < w || S->z1 - S->z0 < d * 0.8f) break;
                    if (overlaps(foot, nf, x - 0.5f * w, z - 0.5f * d, x + 0.5f * w, z + 0.5f * d, S->y)) continue;
                    VItem *it = v_item(v, kind, VR_PROP);
                    if (!it) break;
                    it->p[0] = x; it->p[1] = S->y; it->p[2] = z; it->s[0] = w; it->s[1] = h; it->s[2] = d; it->seed = rng_u32(r);
                    it->rgb = T->goods[it->seed % 3];
                    foot[nf].x0 = x - 0.5f * w; foot[nf].x1 = x + 0.5f * w; foot[nf].z0 = z - 0.5f * d; foot[nf].z1 = z + 0.5f * d; foot[nf].y = S->y; nf++;
                    placed = 1;
                }
            }
        }
        for (int t = 0; t < 8 && !placed && F->nsurf; ++t) {
            const VSurface *S = &F->surf[rng_u32(r) % (uint32_t)F->nsurf];
            if (S->kind == VSU_BED && v->archetype == VA_BARROW && t < 4 && T->goods[0] && v->trade == VT_ROAST) continue;
            if (S->x1 - S->x0 < w || S->z1 - S->z0 < d * 0.8f) continue;
            float x = rng_range(r, S->x0 + 0.5f * w, S->x1 - 0.5f * w), z = clampf(rng_range(r, S->z0 + 0.5f * d, S->z1 - 0.5f * d), S->z0 + 0.4f * d, S->z1 - 0.4f * d);
            if (overlaps(foot, nf, x - 0.5f * w, z - 0.5f * d, x + 0.5f * w, z + 0.5f * d, S->y)) continue;
            VItem *it = v_item(v, kind, VR_PROP);
            if (!it) break;
            it->p[0] = x; it->p[1] = S->y; it->p[2] = z; it->s[0] = w; it->s[1] = h; it->s[2] = d; it->seed = rng_u32(r);
            it->rgb = T->goods[it->seed % 3];
            foot[nf].x0 = x - 0.5f * w; foot[nf].x1 = x + 0.5f * w; foot[nf].z0 = z - 0.5f * d; foot[nf].z1 = z + 0.5f * d; foot[nf].y = S->y; nf++;
            placed = 1;
        }
        if (placed) { k++; np++; }
    }
    /* heaps of loose stock over the tables, tiers and barrow beds (section 7) */
    int loose = v->archetype == VA_MARKET || v->archetype == VA_BARROW;
    int heapCap = v->tier == VTIER_LEAN ? 10 : v->tier == VTIER_STANDARD ? 22 : 36;
    if (G->level == 2) heapCap /= 3;
    int heaps = 0;
    for (int s = 0; s < F->nsurf && loose; ++s) {
        const VSurface *S = &F->surf[s];
        if (S->kind != VSU_TABLE && S->kind != VSU_TIER && S->kind != VSU_BED) continue;
        float area = (S->x1 - S->x0) * (S->z1 - S->z0);
        int nh = (int)(area / 0.06f);
        for (int k = 0, tries = 0; k < nh && heaps < heapCap && tries < nh * 3; ++tries) {
            float hw = rng_range(r, 0.16f, 0.28f), hd = hw * rng_range(r, 0.7f, 1.0f), hh = rng_range(r, 0.07f, 0.15f);
            if (S->x1 - S->x0 < hw || S->z1 - S->z0 < hd) break;
            float x = rng_range(r, S->x0 + 0.5f * hw, S->x1 - 0.5f * hw), z = rng_range(r, S->z0 + 0.5f * hd, S->z1 - 0.5f * hd);
            if (overlaps(foot, nf, x - 0.5f * hw, z - 0.5f * hd, x + 0.5f * hw, z + 0.5f * hd, S->y)) continue;
            VItem *it = v_item(v, VI_HEAP, VR_HEAP);
            if (!it) break;
            it->p[0] = x; it->p[1] = S->y; it->p[2] = z; it->s[0] = hw; it->s[1] = hh; it->s[2] = hd; it->seed = rng_u32(r);
            it->rgb = v_shade(T->goods[it->seed % 3], rng_range(r, 0.8f, 1.1f));
            if (nf < V_MAX_ITEM) { foot[nf].x0 = x - 0.5f * hw; foot[nf].x1 = x + 0.5f * hw; foot[nf].z0 = z - 0.5f * hd; foot[nf].z1 = z + 0.5f * hd; foot[nf].y = S->y; nf++; }
            heaps++; k++;
        }
    }
    /* hanging goods from the rails, clear of heads where people stand (V_HANG_CLEAR) */
    int nhk = 0;
    while (nhk < 3 && T->hang[nhk] != 255) nhk++;
    int hwant = G->level >= 2 ? 0 : (int)((float)TI->hangings * (0.35f + 0.65f * G->clutter) * (G->level ? 0.5f : 1.0f));
    for (int ri = 0, made = 0; ri < F->nrail && made < hwant && nhk; ++ri) {
        const VRail *R = &F->rail[ri];
        float len = sqrtf((R->x1 - R->x0) * (R->x1 - R->x0) + (R->z1 - R->z0) * (R->z1 - R->z0));
        float room = R->outside ? R->y - V_HANG_CLEAR - 0.02f : 1.2f;
        if (room < 0.15f || len < 0.3f) continue;
        int bunting = 0;
        for (int k = 0; k < nhk; ++k) bunting |= T->hang[k] == VI_BUNTING;
        if (bunting && rng_float(r) < 0.5f && room >= hang_len(VI_BUNTING)) {
            VItem *it = v_item(v, VI_BUNTING, VR_HANG);
            if (it) {
                it->p[0] = R->x0; it->p[1] = R->y; it->p[2] = R->z0; it->s[0] = len; it->s[1] = hang_len(VI_BUNTING); it->s[2] = 0.02f;
                it->fx = (R->x1 - R->x0) / len; it->fz = (R->z1 - R->z0) / len; it->flags = VPF_SWAY; it->seed = rng_u32(r);
                made++;
            }
            continue;
        }
        float pitch = rng_range(r, 0.3f, 0.5f);
        for (float u = rng_range(r, 0.1f, pitch); u < len - 0.1f && made < hwant; u += pitch) {
            int kind = T->hang[rng_u32(r) % (uint32_t)nhk];
            if (kind == VI_BUNTING) kind = VI_LANTERN;
            float L = minf(hang_len(kind) * rng_range(r, 0.85f, 1.15f), room);
            if (L < 0.15f) continue;
            VItem *it = v_item(v, kind, VR_HANG);
            if (!it) break;
            it->p[0] = R->x0 + (R->x1 - R->x0) * u / len; it->p[1] = R->y; it->p[2] = R->z0 + (R->z1 - R->z0) * u / len;
            it->s[0] = kind == VI_GARMENT ? 0.45f : 0.2f; it->s[1] = L; it->s[2] = 0.2f;
            it->flags = VPF_SWAY; it->seed = rng_u32(r);
            it->rgb = kind == VI_LANTERN ? (it->seed & 1 ? 0xC8281Eu : 0xE8B030u) : T->goods[it->seed % 3];
            made++;
        }
    }
    /* gas bottles for the cooks: beside the cooking, in a ground slot, never more than V_GAS_MAX in all */
    if (T->cooking == 2 && G->level < 2) {
        int gas = 0;
        for (int i = 0; i < v->nitem; ++i) gas += v->item[i].kind == VI_DGAS;
        int want = 1 + (rng_float(r) < 0.4f);
        for (int k = 0; k < want && gas < V_GAS_MAX; ++k) {
            int si = find_slot(v, VSL_GROUND, 0.35f, 0.95f, 0.35f, r);
            if (si < 0) break;
            VItem *it = slot_item(v, si, VI_GASBOTTLE, VR_GAS, 0.3f, 0.9f, 0.3f);
            if (!it) break;
            it->seed = rng_u32(r);
            it->rgb = (it->seed & 1) ? 0x2E6EA8u : 0xB83A28u;
            gas++;
        }
    }
    /* signs: a roof sign where the roof takes one, a blade sign off a front post */
    float pNeon = v->cls == VC_POOR ? 0.85f : v->cls == VC_MID ? 0.7f : 0.35f;
    float ceil = v->site.ceiling > 0.0f ? v->site.ceiling : 99.0f;
    float rx0 = -0.5f * v->site.w, rx1 = 0.5f * v->site.w, rz1 = v->front;
    if (F->roofPart >= 0) { rx0 = v->part[F->roofPart].a[0]; rx1 = v->part[F->roofPart].b[0]; rz1 = minf(v->part[F->roofPart].b[2], v->site.d); }
    if (F->signY > 0.0f) {
        float w = minf(F->signW, rng_range(r, 1.0f, 2.3f)), h = minf(rng_range(r, 0.45f, 0.9f), V_ROOFSIGN_AREA / w - 0.01f);
        float y = F->signY + 0.12f + 0.5f * h;
        if (y + 0.5f * h < ceil - 0.05f && w > 0.6f) {
            float cx = clampf(rng_range(r, -0.3f, 0.3f), rx0 + 0.5f * w, rx1 - 0.5f * w);
            v_sign(v, VSG_ROOF, cx, y, maxf(rz1 - 0.25f, 0.2f), 0.0f, 1.0f, w, h, rng_float(r) < pNeon);
        }
    }
    if (G->level < 2 && (v->archetype == VA_CANOPY || v->archetype == VA_LEANTO || v->archetype == VA_CONTAINER) && rng_float(r) < (v->cls == VC_POOR ? 0.65f : 0.45f)) {
        float w = rng_range(r, 0.5f, 0.9f), h = rng_range(r, 0.6f, 1.1f), y = V_BLADE_CLEAR + 0.05f + 0.5f * h;
        float side = rng_float(r) < 0.5f ? -1.0f : 1.0f, x = side < 0 ? v->box[0] + 0.1f : v->box[2] - 0.1f;
        /* out from the front corner, beyond the roof's edge, over the footway */
        float z = maxf(v->front, F->roofPart >= 0 ? v->part[F->roofPart].b[2] + 0.02f : v->front) + 0.05f + 0.5f * w;
        if (z + 0.5f * w < v->site.d - 0.02f && y + 0.5f * h < ceil - 0.05f)
            v_sign(v, VSG_BLADE, x, y, z, 1.0f, 0.0f, w, h, rng_float(r) < pNeon);
    }
    /* neon motifs: on the roof beside the sign, else on a free wall */
    int maxNeon = TI->neon;
    for (int i = 0; i < v->nsign; ++i) maxNeon -= v->sign[i].neon;
    int nneon = v->cls == VC_POOR ? rng_int(r, 1, 3) : v->cls == VC_MID ? rng_int(r, 1, 2) : rng_int(r, 0, 1);
    if (v->signature == VSIG_DEADNEON && nneon < 1) nneon = 1;
    if (nneon > maxNeon) nneon = maxNeon < 0 ? 0 : maxNeon;
    if (v->signature == VSIG_DEADNEON && nneon < 1) nneon = 1;
    const VPalette *P = &V_PALETTE[v->palette];
    for (int k = 0; k < nneon && v->nneon < V_MAX_NEON; ++k) {
        float size = rng_range(r, 0.6f, v->cls == VC_POOR ? 1.3f : 0.9f);
        float c[3], nrm[3] = { 0.0f, 0.0f, 1.0f };
        int ok = 0;
        if (k == 0 && v->nsign && v->sign[0].kind == VSG_ROOF) {
            const VSign *s = &v->sign[0];
            float side = s->c[0] > 0.0f ? -1.0f : 1.0f;
            c[0] = s->c[0] + side * (0.5f * s->w + 0.5f * size + 0.08f); c[1] = F->signY + 0.12f + 0.5f * size; c[2] = s->c[2];
            ok = c[0] - 0.5f * size > -0.5f * v->site.w && c[0] + 0.5f * size < 0.5f * v->site.w && c[1] + 0.5f * size < ceil - 0.05f;
        }
        if (!ok) {
            int si = find_slot(v, VSL_WALL, size, size, 0.05f, r);
            if (si >= 0) {
                VSlot *s = &v->slot[si];
                s->used = 1;
                c[0] = s->c[0] + s->n[0] * 0.04f; c[1] = s->c[1] + 0.2f; c[2] = s->c[2] + s->n[2] * 0.04f;
                nrm[0] = s->n[0]; nrm[2] = s->n[2];
                ok = 1;
            }
        }
        if (!ok && F->signY > 0.0f && !v->nsign) {
            c[0] = 0.0f; c[1] = F->signY + 0.12f + 0.5f * size; c[2] = maxf(rz1 - 0.3f, 0.2f);
            ok = c[1] + 0.5f * size < ceil - 0.05f && size < v->site.w;
        }
        if (!ok) continue;
        VNeon *N = &v->neon[v->nneon++];
        memset(N, 0, sizeof(*N));
        uint8_t alt = V_TRADES[v->trade].motifs[rng_u32(r) % 3u];
        N->motif = k == 0 || alt >= VM_COUNT ? v->motif : alt;
        N->color = P->neon[k & 1];
        float fl = rng_float(r);
        N->flicker = (uint8_t)(fl < 0.72f ? VFL_STEADY : fl < 0.85f ? VFL_BUZZ : fl < 0.93f ? VFL_PULSE : VFL_DYING);
        memcpy(N->c, c, sizeof(c)); memcpy(N->n, nrm, sizeof(nrm));
        N->size = size;
    }
    if (v->signature == VSIG_DEADNEON) {
        if (v->nneon) v->neon[0].flicker = VFL_DYING;
        else for (int i = 0; i < v->nsign; ++i) if (v->sign[i].neon) { v->sign[i].flicker = VFL_DYING; break; }
    }
    for (int i = 0; i < v->nsign; ++i) if (v->sign[i].neon && v->sign[i].flicker == VFL_STEADY && rng_float(r) < 0.15f) v->sign[i].flicker = VFL_BUZZ;
}

/* ---------------------------------------------------------- after budget --- */

/* Steam and smoke (section 10.1): one per flue, per cooking prop and per shrine, at most V_EMITTERS_MAX,
 * the tier's particles shared by weight. */
static void emitters(VGen *G) {
    Vendor *v = G->v;
    static const float WEIGHT[VEM_COUNT] = { 1.0f, 1.2f, 1.5f, 0.3f, 0.6f };
    v->nemit = 0;
    int grill = 0;
    for (int i = 0; i < v->nitem; ++i) grill |= v->item[i].kind == VI_GRILL || v->item[i].kind == VI_ROASTER;
    float ceil = v->site.ceiling > 0.0f ? v->site.ceiling - 0.1f : 0.0f;
    for (int i = 0; i < v->npart && v->nemit < V_EMITTERS_MAX; ++i) {
        const VPart *p = &v->part[i];
        if (p->role != VR_FLUE) continue;
        VEmitter *e = &v->emit[v->nemit++];
        memset(e, 0, sizeof(*e));
        e->kind = (uint8_t)(grill ? VEM_SMOKE : VEM_VENT);
        e->pos[0] = p->b[0]; e->pos[1] = p->b[1]; e->pos[2] = p->b[2];
        e->ceiling = ceil;
    }
    for (int i = 0; i < v->nitem && v->nemit < V_EMITTERS_MAX; ++i) {
        const VItem *it = &v->item[i];
        int kind = -1;
        if (it->role == VR_PROP && it->kind < VI_PROP_END && V_PROP[it->kind].emitter) kind = V_PROP[it->kind].emitter - 1;
        if (it->role == VR_SIGNATURE && it->kind == VSIG_SHRINE) kind = VEM_INCENSE;
        if (kind < 0) continue;
        VEmitter *e = &v->emit[v->nemit++];
        memset(e, 0, sizeof(*e));
        e->kind = (uint8_t)kind;
        e->pos[0] = it->p[0]; e->pos[1] = it->p[1] + it->s[1]; e->pos[2] = it->p[2];
        /* under a roof the plume spreads beneath it rather than pouring through */
        e->ceiling = ceil;
        for (int k = 0; k < v->npart; ++k) {
            const VPart *r = &v->part[k];
            if ((r->role != VR_ROOF && r->role != VR_HOOD && r->role != VR_AWNING) || r->a[1] < e->pos[1]) continue;
            if (e->pos[0] < r->a[0] || e->pos[0] > r->b[0] || e->pos[2] < r->a[2] || e->pos[2] > r->b[2]) continue;
            float y = r->a[1] - 0.05f;
            if (e->ceiling <= 0.0f || y < e->ceiling) e->ceiling = y;
        }
    }
    float wsum = 0.0f;
    for (int i = 0; i < v->nemit; ++i) wsum += WEIGHT[v->emit[i].kind];
    uint32_t tint = v->nneon ? 0u : V_TRADES[v->trade].lamp;
    if (v->nneon) {
        /* a puff beside a sign takes its colour: the first neon's, softened towards the lamp */
        static const uint32_t NEON[8] = { 0xFF1480u, 0x0DD9FFu, 0xFF1A0Au, 0xFF8010u, 0x8C1FFFu, 0x26FF66u, 0xA6D9FFu, 0xFFD926u };
        uint32_t a = NEON[v->neon[0].color & 7], b = V_TRADES[v->trade].lamp;
        tint = v_rgb((float)(((a >> 16) & 255) + 2 * ((b >> 16) & 255)) / 765.0f, (float)(((a >> 8) & 255) + 2 * ((b >> 8) & 255)) / 765.0f,
                     (float)((a & 255) + 2 * (b & 255)) / 765.0f);
    }
    v->st.particles = 0;
    for (int i = 0; i < v->nemit; ++i) {
        VEmitter *e = &v->emit[i];
        const VEmitInfo *I = &V_EMIT[e->kind];
        e->count = wsum > 0.0f ? (int)((float)V_TIER[v->tier].particles * WEIGHT[e->kind] / wsum) : 0;
        e->life = I->life; e->size0 = I->size0; e->size1 = I->size1; e->rise = I->rise; e->opacity = I->opacity;
        e->spread = I->spread; e->turbulence = I->turbulence; e->tint = tint;
        v->st.particles += e->count;
    }
}

/* Lanterns give a little light each, while there are lights to spare. */
static void lantern_lights(Vendor *v) {
    for (int i = 0; i < v->nitem && v->nlight < V_MAX_LIGHT; ++i) {
        const VItem *it = &v->item[i];
        if (it->kind != VI_LANTERN || it->role != VR_HANG) continue;
        v_light(v, VLK_LANTERN, it->p[0], it->p[1] - 0.5f * it->s[1], it->p[2], 0.15f, it->rgb);
    }
}

static void bounds(Vendor *v) {
    float x0 = 1e30f, z0 = 1e30f, x1 = -1e30f, z1 = -1e30f, top = 0.0f;
    for (int i = 0; i < v->npart; ++i) {
        float mn[3], mx[3];
        v_part_aabb(&v->part[i], mn, mx);
        x0 = minf(x0, mn[0]); x1 = maxf(x1, mx[0]); z0 = minf(z0, mn[2]); z1 = maxf(z1, mx[2]); top = maxf(top, mx[1]);
    }
    for (int i = 0; i < v->nitem; ++i) {
        const VItem *it = &v->item[i];
        if (it->kind == VI_BUNTING) continue;
        /* its footprint turned to its facing: across (s[0]) along (fz, -fx), deep (s[2]) along (fx, fz) */
        float ex = 0.5f * (fabsf(it->fz) * it->s[0] + fabsf(it->fx) * it->s[2]), ez = 0.5f * (fabsf(it->fx) * it->s[0] + fabsf(it->fz) * it->s[2]);
        x0 = minf(x0, it->p[0] - ex); x1 = maxf(x1, it->p[0] + ex); z0 = minf(z0, it->p[2] - ez); z1 = maxf(z1, it->p[2] + ez);
        if (it->role != VR_HANG) top = maxf(top, it->p[1] + it->s[1]);
    }
    for (int i = 0; i < v->nsign; ++i) top = maxf(top, v->sign[i].c[1] + 0.5f * v->sign[i].h);
    for (int i = 0; i < v->nneon; ++i) top = maxf(top, v->neon[i].c[1] + 0.5f * v->neon[i].size);
    v->box[0] = x0; v->box[1] = z0; v->box[2] = x1; v->box[3] = z1; v->top = top;
}

/* The reduction order (section 9.2), until the vendor is within its tier's triangles. */
static void reduce(VGen *G) {
    Vendor *v = G->v;
    int cap = V_TIER[v->tier].tris;
    int tris = vendor_tris(v, -1);
    int idle = 0;
    for (int step = 0; tris > cap && idle < 7; step = (step + 1) % 7) {
        int did = 0;
        switch (step) {
        case 0:   /* a decoration */
            for (int i = v->nitem - 1; i >= 0; --i) if (v->item[i].role == VR_DECOR) { vlog(G, "    simplify: drop the %s\n", vendor_item_name(v->item[i].kind)); remove_item(v, i); did = 1; break; }
            break;
        case 1: { /* two hanging items */
            int n = 0;
            for (int i = v->nitem - 1; i >= 0 && n < 2; --i) if (v->item[i].role == VR_HANG) { remove_item(v, i); n++; }
            if (n) { vlog(G, "    simplify: remove %d hanging items\n", n); did = 1; }
            break;
        }
        case 2:   /* a counter item (or three heaps, which are counter items too) */
            for (int i = v->nitem - 1; i >= 0; --i) {
                if (v->item[i].role == VR_HEAP) {
                    int n = 0;
                    for (int k = v->nitem - 1; k >= 0 && n < 3; --k) if (v->item[k].role == VR_HEAP) { remove_item(v, k); n++; }
                    vlog(G, "    simplify: remove %d heaps\n", n); did = 1; break;
                }
                if (v->item[i].role == VR_PROP) { vlog(G, "    simplify: remove the %s\n", vendor_item_name(v->item[i].kind)); remove_item(v, i); did = 1; break; }
            }
            break;
        case 3:
            if (v->detail > 0) { v->detail--; vlog(G, "    simplify: lower the detail to %d\n", v->detail); did = 1; }
            break;
        case 4:
            for (int i = v->nitem - 1; i >= 0; --i) if (v->item[i].role == VR_STOOL) { vlog(G, "    simplify: drop a stool\n"); remove_item(v, i); did = 1; break; }
            break;
        case 5:
            if (v->signature != VSIG_DEADNEON)
                for (int i = v->nitem - 1; i >= 0; --i) if (v->item[i].role == VR_SIGNATURE) { vlog(G, "    simplify: drop the signature (%s)\n", vendor_signature_name(v->signature)); remove_item(v, i); did = 1; break; }
            break;
        case 6: {
            int n = 0;
            for (int i = v->nitem - 1; i >= 0; --i) if (v->item[i].role == VR_HANG) { remove_item(v, i); n++; }
            if (n) { vlog(G, "    simplify: remove all hanging items\n"); did = 1; }
            break;
        }
        }
        if (did) { v->st.reductions++; idle = 0; tris = vendor_tris(v, -1); }
        else idle++;
    }
}

static void stats(Vendor *v) {
    VendorStats *s = &v->st;
    s->tris = vendor_tris(v, -1); s->shellTris = vendor_tris(v, VL_SHELL); s->dressTris = vendor_tris(v, VL_DRESSING);
    s->props = s->hangings = s->stools = s->heaps = s->decor = 0;
    for (int i = 0; i < v->nitem; ++i) {
        switch (v->item[i].role) {
        case VR_PROP:  s->props++; break;
        case VR_HANG:  s->hangings++; break;
        case VR_STOOL: s->stools++; break;
        case VR_HEAP:  s->heaps++; break;
        case VR_DECOR: s->decor++; break;
        default: break;
        }
    }
    s->neon = v->nneon; s->signs = v->nsign; s->lights = v->nlight; s->emitters = v->nemit;
    for (int i = 0; i < v->nsign; ++i) s->neon += v->sign[i].neon;
}

/* ---------------------------------------------------------------- generate --- */

int vendor_generate(const VendorSite *site, int tier, const VendorFinger *ctx, int n, Vendor *v, BLog *log) {
    VGen G;
    memset(&G, 0, sizeof(G));
    G.log = log;
    int cls = vendor_class(site->affluence);
    Rng ra = rng_make(hash_combine(site->seed, 0xA4C7u));
    int trade = pick_trade(site, cls, &ra);
    float noise = rng_float(&ra);
    blog_printf(log, "vendor %08x: %s site %.1f x %.1f m, affluence %.2f (%s)%s, %s tier\n", site->seed, vendor_site_name(site->kind),
                site->w, site->d, site->affluence, vendor_class_name(cls), site->lunch ? ", lunch" : "", vendor_tier_name(tier));
    int ok = 0;
    for (int level = 0; level <= 2 && !ok; ++level) {
        memset(v, 0, sizeof(*v));
        v->site = *site; v->tier = tier; v->cls = cls; v->trade = trade; v->detail = 2;
        v->cooking = V_TRADES[trade].cooking;
        v->wear = clampf(1.0f - site->affluence + (noise - 0.5f) * 0.35f, 0.0f, 1.0f);
        G.v = v; G.level = level;
        G.clutter = clampf(0.2f + 0.8f * (1.0f - site->affluence) + (noise - 0.5f) * 0.25f, 0.0f, 1.0f);
        /* archetype and structure: the likeliest that fits, the next if it doesn't */
        float aw[VA_COUNT];
        for (int a = 0; a < VA_COUNT; ++a) aw[a] = site->archetype >= 0 ? (a == site->archetype && fits(site, a) ? 1.0f : 0.0f) : arch_weight(site, cls, trade, a);
        Rng rsel = rng_make(hash_combine(site->seed, 0xA4C8u + (uint32_t)level));
        VFrame F;
        int built = 0;
        for (int tries = 0; tries < VA_COUNT && !built; ++tries) {
            int a = v_pick(&rsel, aw, VA_COUNT);
            if (a < 0) break;
            int keepItems = 0;
            v->npart = v->nedge = v->nopen = v->nsign = v->nneon = v->nlight = v->nemit = v->nslot = v->nitem = keepItems;
            v->ncbox = 0; v->roofY = 0.0f; v->court = 0.0f; v->layout = v->variant = 0;
            v->archetype = a;
            G.rs = rng_make(hash3i(site->seed ^ 0x57C7u, a, level, 0));
            built = v_build_structure(&G, &F);
            if (!built) { vlog(&G, "  reject: a %s doesn't fit the site\n", vendor_archetype_name(a)); aw[a] = 0.0f; }
        }
        if (!built) {
            vlog(&G, "  reject: nothing fits the site\n");
            v->ok = 0;
            return 0;
        }
        v->nstructure = v->npart;
        int structItems = v->nitem, structSlots = v->nslot, structNeon = v->nneon;
        vlog(&G, "  level %d: %s, %s, %s, %d parts\n", level, vendor_archetype_name(v->archetype), vendor_trade_name(trade), vendor_class_name(cls), v->npart);
        /* dressing, rerolled while too like a neighbour */
        int dressed = 0, worst = 0;
        for (int attempt = 0; attempt <= V_DRESS_REROLLS && !dressed; ++attempt) {
            v->nitem = structItems; v->nneon = structNeon;
            for (int s = 0; s < structSlots; ++s) v->slot[s].used = 0;
            Rng rd = rng_make(hash3i(site->seed ^ 0xD2E55u, level, attempt, 1));
            G.rd = rd;
            if (!dress(&G, &F, ctx, n, &G.rd)) { vlog(&G, "  reroll: no signature fits\n"); v->st.rerolls++; continue; }
            if (too_alike(v, ctx, n, &worst)) { vlog(&G, "  reroll: shares %d of 9 with a neighbour\n", worst); v->st.rerolls++; continue; }
            dressed = 1;
        }
        if (!dressed) {
            /* force a change (section 8): the palette first, as the doc asks, then the feature wall,
             * the motif and the finish, each set to whichever value leaves the closest neighbour
             * furthest, until none shares more than V_FINGER_SHARE */
            uint8_t *axis[5] = { &v->palette, &v->feature, &v->motif, &v->finish, &v->stock[2] };
            static const int COUNT[5] = { V_PALETTES, VW_COUNT, VM_COUNT, VF_COUNT, VK_COUNT };
            static const char *const NAME[5] = { "palette", "feature wall", "motif", "finish", "sideline stock" };
            v->st.forced = 1;
            for (int pass = 0; pass < 2 && too_alike(v, ctx, n, &worst); ++pass)
                for (int ax = 0; ax < 5 && too_alike(v, ctx, n, &worst); ++ax) {
                    /* the value that leaves the least excess over all the neighbours together */
                    uint8_t best = *axis[ax];
                    int bestCost = 1 << 30;
                    for (int k = 0; k < COUNT[ax]; ++k) {
                        *axis[ax] = (uint8_t)k;
                        v->shelfMix = (uint8_t)(hash_combine(hash_combine(v->stock[0], v->stock[1]), v->stock[2]) & 255u);
                        fingerprint(v);
                        int cost = 0;
                        for (int i = 0; i < n; ++i) {
                            int sh = vendor_finger_shared(&v->finger, &ctx[i]);
                            cost += (sh > V_FINGER_SHARE ? 100 * (sh - V_FINGER_SHARE) : 0) + sh;
                        }
                        if (cost < bestCost) { bestCost = cost; best = (uint8_t)k; }
                    }
                    *axis[ax] = best;
                    v->shelfMix = (uint8_t)(hash_combine(hash_combine(v->stock[0], v->stock[1]), v->stock[2]) & 255u);
                    fingerprint(v);
                    too_alike(v, ctx, n, &worst);
                    vlog(&G, "  forced: %s %d (shares at most %d)\n", NAME[ax], best, worst);
                }
        }
        /* the structure's signs were made before the palette was chosen */
        for (int i = 0; i < v->nsign; ++i) v->sign[i].color = V_PALETTE[v->palette].neon[i & 1];
        G.rf = rng_make(hash3i(site->seed ^ 0xF111u, level, 0, 2));
        fill(&G, &F, &G.rf);
        fingerprint(v);
        reduce(&G);
        lantern_lights(v);
        emitters(&G);
        fingerprint(v);
        bounds(v);
        stats(v);
        v->st.level = level;
        int errors = vendor_validate(v, ctx, n, log);
        ok = errors == 0;
        v->ok = ok;
        vlog(&G, "  %s: %d triangles (shell %d, dressing %d), %d props, %d heaps, %d hangings, %d stools, %d decorations, %d neon, %d emitters\n",
             ok ? "ok" : "reject", v->st.tris, v->st.shellTris, v->st.dressTris, v->st.props, v->st.heaps, v->st.hangings, v->st.stools, v->st.decor, v->st.neon, v->st.emitters);
    }
    return ok;
}
