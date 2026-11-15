#include "vendor_internal.h"

/* ===========================================================================
 * Structure (section 5 of docs/street-vendor-generation.md): every form is
 * built on the kit from its plan (src/gen/noodle_bar.c, Vendor.nb); its parts
 * here mirror that plan for the bounded rules (section 4), which the validator
 * (vendor_validate.c) checks from the parts alone. (The doc's lean-to,
 * container, pair, market stall and barrow builders were retired in SF6.)
 *
 * Each builder also leaves the fill stage what it needs (VFrame): the tops
 * things stand on, the rails things hang from, where the cooking is, where a
 * roof sign may stand; and slots for the decorations (section 7).
 * ========================================================================= */

static float ceiling_of(const Vendor *v) { return v->site.ceiling > 0.0f ? v->site.ceiling : 99.0f; }

static void add_surface(VFrame *F, int kind, float x0, float z0, float x1, float z1, float y) {
    if (F->nsurf >= V_MAX_SURF || x1 - x0 < 0.15f || z1 - z0 < 0.12f) return;
    VSurface *s = &F->surf[F->nsurf++];
    s->x0 = x0; s->z0 = z0; s->x1 = x1; s->z1 = z1; s->y = y; s->kind = (uint8_t)kind;
}

static void add_rail(VFrame *F, float x0, float z0, float x1, float z1, float y, int outside) {
    if (F->nrail >= V_MAX_RAIL) return;
    VRail *r = &F->rail[F->nrail++];
    r->x0 = x0; r->z0 = z0; r->x1 = x1; r->z1 = z1; r->y = y; r->outside = (uint8_t)outside;
}

/* An upright post from the ground (or y0) to y1. */
static int post(Vendor *v, int surf, int col, float x, float z, float y0, float y1, float r, int round) {
    if (round) return v_cyl(v, VR_SUPPORT, surf, col, x, y0, z, x, y1, z, r, 8);
    int p = v_box(v, VR_SUPPORT, surf, col, x - r, y0, z - r, x + r, y1, z + r);
    if (p >= 0) v->part[p].faces = VB_BOT;
    return p;
}

/* Posts along x from xa to xb at z, no bay wider than V_SPAN_MAX. */
static int post_row(Vendor *v, int surf, int col, float xa, float xb, float z, float y1, float r, int round, int roof, int *out, int max) {
    int bays = (int)ceilf((xb - xa) / (V_SPAN_MAX - 0.05f));
    if (bays < 1) bays = 1;
    int n = 0;
    for (int k = 0; k <= bays && (!out || n < max); ++k) {
        float x = xa + (xb - xa) * (float)k / (float)bays;
        int p = post(v, surf, col, x, z, 0.0f, y1, r, round);
        if (p >= 0 && roof >= 0) v_edge(v, p, roof, VE_SUPPORTS);
        if (out) out[n] = p;
        n++;
    }
    return n;
}

static uint32_t lamp_rgb(const Vendor *v) {
    uint32_t c = V_TRADES[v->trade].lamp;
    if (v->cls == VC_POOR) {   /* cold, greenish tubes */
        float r = (float)((c >> 16) & 255u), g = (float)((c >> 8) & 255u), b = (float)(c & 255u);
        return v_rgb((r * 0.5f + 205.0f * 0.5f) / 255.0f, (g * 0.5f + 240.0f * 0.5f) / 255.0f, (b * 0.5f + 225.0f * 0.5f) / 255.0f);
    }
    if (v->cls == VC_RICH) {   /* warm white LED, as ref 3 */
        float r = (float)((c >> 16) & 255u), g = (float)((c >> 8) & 255u), b = (float)(c & 255u);
        return v_rgb((r * 0.4f + 255.0f * 0.6f) / 255.0f, (g * 0.4f + 212.0f * 0.6f) / 255.0f, (b * 0.4f + 160.0f * 0.6f) / 255.0f);
    }
    return c;
}
static int lamp_kind(const Vendor *v) { return v->cls == VC_RICH ? VLK_LED : VLK_TUBE; }

/* A run of stools at pitch along a line from (xa, za) to (xb, zb), facing (fx, fz). */
static int stool_row(Vendor *v, Rng *r, float xa, float za, float xb, float zb, float fx, float fz, int most) {
    float len = sqrtf((xb - xa) * (xb - xa) + (zb - za) * (zb - za));
    float pitch = rng_range(r, 0.66f, 0.82f);
    int n = (int)floorf((len - 0.2f) / pitch) + 1;
    if (n > most) n = most;
    if (n < 1 || len < 0.4f) return 0;
    float used = (float)(n - 1) * pitch, s0 = 0.5f * (len - used);
    float h = rng_range(r, 0.68f, 0.78f);
    int made = 0;
    for (int k = 0; k < n; ++k) {
        float t = (s0 + pitch * (float)k) / len;
        VItem *it = v_item(v, VI_STOOL, VR_STOOL);
        if (!it) break;
        it->p[0] = xa + (xb - xa) * t; it->p[1] = 0.0f; it->p[2] = za + (zb - za) * t;
        it->s[0] = 0.36f; it->s[1] = h; it->s[2] = 0.36f;
        it->fx = fx; it->fz = fz;
        it->seed = rng_u32(r);
        made++;
    }
    return made;
}

/* A box-shaped wall, its finish the vendor's. */
static int wall(Vendor *v, float x0, float y0, float z0, float x1, float y1, float z1) {
    return v_box(v, VR_SHELL, VS_FINISH, VCOL_BODY, x0, y0, z0, x1, y1, z1);
}

/* A roof sign's stand and the roof's slots: n roof slots along x on the roof's top at y. */
static void roof_slots(Vendor *v, float x0, float x1, float z0, float z1, float y, int n) {
    for (int k = 0; k < n; ++k) {
        float x = x0 + (x1 - x0) * ((float)k + 0.5f) / (float)n;
        v_slot(v, VSL_ROOF, x, y, 0.5f * (z0 + z1), 0.0f, 1.0f, (x1 - x0) / (float)n - 0.1f, 1.5f);
    }
}

/* ------------------------------------------------------- canopy noodle bar --- */

/* The canopy noodle bar is made from its plan (src/gen/noodle_bar.c, roadmap NB4): the city draws the plan's
 * body and far shell, never these parts. They mirror the plan coarsely, so the stages after this one (the
 * rules, the bounds, the footprint, the dressing's tops and slots) see the bar as it is built: the hood's legs
 * and the deck as roofs carried by the walls, posts and column; the counter's runs; the bench; the extractor
 * and its duct; the stools where the plan sets them; the tubes as its lights. */
/* SF2: every form on the kit (noodle_bar.h) is planned the same way and mirrored in coarse parts that are never
 * drawn (the rules, bounds and dressing see the vendor through them): the canopy bar (straight or corner, form
 * -1: the plan chooses), the hatch kiosk, the arcade counter, the tiered kiosk. */
static int build_kit(VGen *G, VFrame *F, int form) {
    Vendor *v = G->v;
    float ceil = ceiling_of(v);
    NbPlan *p = &v->nb;
    if (!nb_plan_site(p, v->site.w, v->site.d, v->site.ceiling, v->site.seed, form, v->trade, v->site.affluence,
                      (v->site.hostWall ? NB_SITE_HOST : 0) | (v->site.vaulted ? NB_SITE_VAULT : 0) | (v->site.deep ? NB_SITE_DEEP : 0) |
                      (v->site.corner > 0 ? NB_SITE_CORNER_POS : v->site.corner < 0 ? NB_SITE_CORNER_NEG : 0) |
                      (v->site.kind == VSITE_ARCOLOGY ? NB_SITE_ARC : 0), v->site.recess)) return 0;
    if (p->top > ceil + 1e-3f) return 0;
    v->layout = p->form;
    v->variant = p->side > 0;
    v->front = p->front;
    v->clearMin = V_CLEAR_MIN;
    const PmHood *h = &p->hood;
    v->roofY = h->spring;
    /* walls and posts, which carry the roofs */
    int sup[NB_WALLS_MAX + NB_POSTS_MAX + 1], ns = 0;
    for (int i = 0; i < p->nwall; ++i) {
        const NbWall *w = &p->wall[i];
        float ax = w->o[0], az = w->o[1], bx = ax + w->u[0] * w->len, bz = az + w->u[1] * w->len;
        float nx = -w->u[1], nz = w->u[0];   /* outward */
        int k = wall(v, minf(ax, bx), 0.0f, minf(az, bz), maxf(ax, bx), w->h, maxf(az, bz));
        if (k >= 0) {
            VPart *q = &v->part[k];
            /* 4 cm thick, inward of its face */
            if (fabsf(w->u[0]) < 0.5f) { q->a[0] = nx > 0.0f ? ax - 0.04f : ax; q->b[0] = q->a[0] + 0.04f; }
            else { q->a[2] = nz > 0.0f ? az - 0.04f : az; q->b[2] = q->a[2] + 0.04f; }
            sup[ns++] = k;
        }
    }
    for (int i = 0; i < p->npost; ++i) {
        int k = post(v, VS_STEEL, VCOL_TRIM, p->post[i][0], p->post[i][1], 0.0f, p->post[i][2], 0.03f, 0);
        if (k >= 0) sup[ns++] = k;
    }
    if (p->column[2] > 0.0f) {
        int k = v_box(v, VR_SUPPORT, VS_STEEL, VCOL_TRIM, p->column[0] - p->column[2], 0.0f, p->column[1] - p->column[2],
                      p->column[0] + p->column[2], p->column[3], p->column[1] + p->column[2]);
        if (k >= 0) sup[ns++] = k;
    }
    /* the roofs: each leg of the hood as a slab from its spine to its lip, the deck */
    int roof = -1;
    float s0 = 0.0f;
    for (int j = 0; j + 1 < h->nspine; ++j) {
        float ax = h->spine[j][0], az = h->spine[j][1], bx = h->spine[j + 1][0], bz = h->spine[j + 1][1];
        float dx = bx - ax, dz = bz - az, L = sqrtf(dx * dx + dz * dz);
        if (L < 1e-3f) continue;
        /* the leg's spine and its lip, mitred round a corner (the hip goes with both legs) */
        float la[3], lb[3];
        pm_hood_point(h, s0, 1.0f, la);
        pm_hood_point(h, s0 + L, 1.0f, lb);
        s0 += L;
        int k = v_box(v, VR_ROOF, VS_RUST, VCOL_BODY, minf(minf(ax, bx), minf(la[0], lb[0])), h->spring,
                      minf(minf(az, bz), minf(la[2], lb[2])), maxf(maxf(ax, bx), maxf(la[0], lb[0])), p->hoodTop,
                      maxf(maxf(az, bz), maxf(la[2], lb[2])));
        if (k < 0) continue;
        if (roof < 0) roof = k;
        for (int i = 0; i < ns; ++i) v_edge(v, sup[i], k, VE_SUPPORTS);
    }
    const int legs = v->npart;   /* the hood's legs' roofs are the parts before it */
    if (p->deck[2] > p->deck[0]) {
        int k = v_box(v, VR_ROOF, VS_RUST, VCOL_BODY, p->deck[0], p->hoodTop - 0.02f, p->deck[1], p->deck[2], p->hoodTop, p->deck[3]);
        if (k >= 0) for (int i = 0; i < ns; ++i) v_edge(v, sup[i], k, VE_SUPPORTS);
        /* SF2: the tiered kiosk's deck sits on its hood's spine all round (the legs carry its edges) */
        if (k >= 0 && p->form == NB_TIERED)
            for (int i = 0; i < legs; ++i) if (v->part[i].role == VR_ROOF) v_edge(v, i, k, VE_SUPPORTS);
        if (roof < 0) roof = k;
    }
    if (p->form == NB_ARCADE && p->rodTop > 0.0f && roof >= 0) {   /* SF2: the arcade's deck hangs from the ceiling */
        const int nr = p->x1 - p->x0 > 3.0f ? 3 : 2;
        for (int r = 0; r < nr; ++r) {
            const float x = r == 0 ? p->x0 + 0.15f : r == 1 ? p->x1 - 0.15f : 0.5f * (p->x0 + p->x1), z = p->lip - 0.03f;
            int k = v_box(v, VR_SUPPORT, VS_STEEL, VCOL_TRIM, x - 0.008f, p->deckY, z - 0.008f, x + 0.008f, p->rodTop, z + 0.008f);
            if (k >= 0) v_edge(v, k, roof, VE_SUPPORTS);
        }
    }
    if (p->upper[2] > p->upper[0]) {   /* SF2: the tiered kiosk's upper tier on its deck */
        int k = v_box(v, VR_SHELL, VS_STEEL, VCOL_BODY, p->upper[0], p->upper[4], p->upper[1], p->upper[2], p->upper[5], p->upper[3]);
        if (k >= 0 && roof >= 0) v_edge(v, roof, k, VE_SUPPORTS);
    }
    F->roofTop = p->hoodTop; F->roofPart = roof;
    /* the counter's runs (their tops), the bench, the extractor, the duct */
    for (int i = 0; i < p->nrun; ++i) {
        const NbRun *r = &p->run[i];
        float ax = r->o[0], az = r->o[1], bx = ax + r->u[0] * r->len, bz = az + r->u[1] * r->len;
        float nx = -r->u[1] * p->cd, nz = r->u[0] * p->cd;   /* back from the face */
        float x0 = minf(minf(ax, bx), minf(ax - nx, bx - nx)), x1 = maxf(maxf(ax, bx), maxf(ax - nx, bx - nx));
        float z0 = minf(minf(az, bz), minf(az - nz, bz - nz)), z1 = maxf(maxf(az, bz), maxf(az - nz, bz - nz));
        int body = v_box(v, VR_SHELL, VS_STEEL, VCOL_ACC2, x0, 0.0f, z0, x1, p->cy - 0.04f, z1);
        /* SF2: the kiosk's counter is its inner counter and the sill outside it together */
        const float zs = p->form == NB_HATCH && i == 0 ? z1 + p->sill : z1;
        int top = v_box(v, VR_COUNTER, VS_STAINLESS, VCOL_TRIM, x0, p->cy - 0.04f, z0, x1, p->cy, zs);
        if (body >= 0 && top >= 0) v_edge(v, body, top, VE_SUPPORTS);
        add_surface(F, VSU_COUNTER, x0 + 0.05f, z0 + 0.05f, x1 - 0.05f, z1 - 0.05f, p->cy);
    }
    if (p->benchX1 - p->benchX0 > 0.3f) {
        v_box(v, VR_SHELL, VS_STAINLESS, VCOL_TRIM, p->benchX0, 0.0f, p->lining, p->benchX1, p->benchY, p->lining + p->benchD);
        add_surface(F, VSU_COUNTER, p->benchX0 + 0.05f, p->lining + 0.03f, p->benchX1 - 0.05f, p->lining + p->benchD - 0.03f, p->benchY);
    }
    int hood = v_box(v, VR_HOOD, VS_STAINLESS, VCOL_TRIM, p->rangeX0 - 0.1f, 1.97f, p->lining, p->rangeX1 + 0.1f, 2.40f, p->lining + 0.6f);
    /* SF2: a tiered kiosk's lantern is its flue's outlet (the duct ends inside it, the lantern vents it) */
    const float flueTop = p->form == NB_TIERED && p->upperKind == NB_UPPER_LANTERN ? p->upper[5] + 0.1f : p->ductTop;
    int flue = v_cyl(v, VR_FLUE, VS_STEEL, VCOL_TRIM, p->ductX, 2.40f, p->ductZ, p->ductX, flueTop, p->ductZ, p->ductR, 8);
    if (flue >= 0 && hood >= 0) v_edge(v, flue, hood, VE_ATTACHED_TO);
    if (hood >= 0 && roof >= 0) v_edge(v, hood, roof, VE_MOUNTED_ON);
    F->cook[0] = 0.5f * (p->rangeX0 + p->rangeX1); F->cook[1] = p->benchY; F->cook[2] = p->lining + 0.5f * p->benchD;
    F->front = p->front;
    F->signY = 0.0f; F->signW = 0.0f;   /* the fascia's icons are the bar's signs (NB6) */
    /* the stools where the plan sets them, facing the counter */
    for (int i = 0; i < p->nstool; ++i) {
        VItem *it = v_item(v, VI_STOOL, VR_STOOL);
        if (!it) break;
        it->p[0] = p->stool[i][0]; it->p[1] = 0.0f; it->p[2] = p->stool[i][1];
        it->s[0] = 0.36f; it->s[1] = 0.75f; it->s[2] = 0.36f;
        it->fx = 0.0f; it->fz = -1.0f;
        it->seed = hash_combine(v->site.seed, 0x5700u + (uint32_t)i);
    }
    v->st.stools = p->nstool;
    /* the tubes, and a warm lamp over the kitchen */
    uint32_t lc = lamp_rgb(v);
    for (int i = 0; i < p->ntube; ++i) {
        const float *t = p->tube[i];
        v_light(v, VLK_TUBE, 0.5f * (t[0] + t[3]), t[1], 0.5f * (t[2] + t[5]), 1.0f, lc);
    }
    v_light(v, VLK_BULB, F->cook[0], 1.9f, p->lining + 0.4f, 0.6f, 0xFFB46Bu);
    /* slots: the side walls' outer faces for a poster or a meter, the ground beside the bar */
    for (int i = 0; i < p->nwall; ++i) {
        const NbWall *w = &p->wall[i];
        if (fabsf(w->u[0]) > 0.5f || w->len < 0.8f) continue;
        float nx = -w->u[1];
        v_slot(v, VSL_WALL, w->o[0] + nx * 0.005f, 1.4f, w->o[1] + w->u[1] * 0.5f * w->len, nx, 0.0f, w->len - 0.3f, 1.2f);
    }
    float sw = 0.5f * (v->site.w - (p->x1 - p->x0));
    if (sw > 0.8f && !p->deep) {   /* (SF3: the deep unit's room has its tables there) */
        v_slot(v, VSL_GROUND, p->x0 - 0.5f * sw, 0.0f, 0.5f * p->front, 0.0f, 1.0f, sw - 0.1f, 1.6f);
        v_slot(v, VSL_GROUND, p->x1 + 0.5f * sw, 0.0f, 0.5f * p->front, 0.0f, 1.0f, sw - 0.1f, 1.6f);
    }
    return 1;
}

int v_build_structure(VGen *G, VFrame *F) {
    memset(F, 0, sizeof(*F));
    F->cook[1] = -1.0f;
    F->roofPart = -1;
    switch (G->v->archetype) {
    case VA_CANOPY:    return build_kit(G, F, -1);
    case VA_HATCH:     return build_kit(G, F, NB_HATCH);
    case VA_ARCADE:    return build_kit(G, F, NB_ARCADE);
    case VA_TIERED:    return build_kit(G, F, NB_TIERED);
    default:           return 0;
    }
}

void vendor_min_site(int a, float *w, float *d) {
    static const float M[VA_COUNT][2] = { { 2.2f, 2.4f }, { 1.9f, 1.65f }, { 2.2f, 2.2f }, { 4.0f, 3.4f } };   /* canopy, hatch, arcade, tiered */
    *w = a >= 0 && a < VA_COUNT ? M[a][0] : 2.0f;
    *d = a >= 0 && a < VA_COUNT ? M[a][1] : 2.0f;
}
