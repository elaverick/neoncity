#include "vendor_internal.h"

/* ===========================================================================
 * Structure (section 5 of docs/street-vendor-generation.md): the shell of
 * each archetype, its roof or canopy, counter or table or bed, openings and
 * supports, kept within the bounded rules (section 4) by construction; the
 * validator (vendor_validate.c) checks them again from the parts alone.
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
                      (v->site.hostWall ? NB_SITE_HOST : 0) | (v->site.vaulted ? NB_SITE_VAULT : 0))) return 0;
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
    int flue = v_cyl(v, VR_FLUE, VS_STEEL, VCOL_TRIM, p->ductX, 2.40f, p->ductZ, p->ductX, p->ductTop, p->ductZ, p->ductR, 8);
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
    if (sw > 0.8f) {
        v_slot(v, VSL_GROUND, p->x0 - 0.5f * sw, 0.0f, 0.5f * p->front, 0.0f, 1.0f, sw - 0.1f, 1.6f);
        v_slot(v, VSL_GROUND, p->x1 + 0.5f * sw, 0.0f, 0.5f * p->front, 0.0f, 1.0f, sw - 0.1f, 1.6f);
    }
    return 1;
}

/* ---------------------------------------------------------------- lean-to --- */

static int build_leanto(VGen *G, VFrame *F) {
    Vendor *v = G->v;
    Rng *r = &G->rs;
    float W = v->site.w, D = v->site.d, ceil = ceiling_of(v);
    if (!v->site.hostWall || W < 2.4f || D < 2.3f || ceil < 3.0f) return 0;
    float lw = minf(W - 0.1f, rng_range(r, 2.5f, 6.0f)), dp = minf(D - 0.45f, rng_range(r, 2.0f, 4.0f));
    if (dp < 1.9f) return 0;
    float yHi = minf(rng_range(r, 2.95f, 3.4f), ceil - 0.15f), fall = rng_range(r, 0.3f, 0.6f);
    float yLo = yHi - fall;
    if (yLo < 2.42f) { yLo = 2.42f; fall = yHi - yLo; }
    if (fall < V_FALL_MIN + 0.02f) return 0;
    float ov = minf(rng_range(r, 0.15f, 0.5f), D - dp - 0.05f);
    ov = minf(ov, (yLo - V_CLEAR_MIN - 0.02f) * dp / fall);   /* the sheet's edge stays over the clearance */
    if (ov < 0.0f) ov = 0.0f;
    float x0 = -0.5f * lw, x1 = 0.5f * lw, zF = dp + ov, yF = yHi - fall * zF / dp;
    v->clearMin = V_CLEAR_MIN;
    /* the ledger on the host wall and the sheet down to the posts */
    int ledger = v_box(v, VR_SUPPORT, VS_TIMBER, VCOL_TRIM, x0, yHi - 0.14f, 0.0f, x1, yHi, 0.08f);
    float q[4][3] = { { x0, yHi, 0.0f }, { x1, yHi, 0.0f }, { x1, yF, zF }, { x0, yF, zF } };
    int roof = v_quad(v, VR_ROOF, v->cls == VC_POOR && rng_float(r) < 0.4f ? VS_TARP : VS_CORRUGATED, v->cls == VC_POOR ? VCOL_ACC1 : VCOL_BODY, q, VPF_TWOSIDED);
    if (ledger >= 0 && roof >= 0) v_edge(v, ledger, roof, VE_SUPPORTS);
    int round = rng_float(r) < 0.3f;
    post_row(v, VS_TIMBER, VCOL_TRIM, x0 + 0.06f, x1 - 0.06f, dp, yLo, 0.05f, round, roof, NULL, 0);
    int beam = v_box(v, VR_SUPPORT, VS_TIMBER, VCOL_TRIM, x0, yLo - 0.12f, dp - 0.05f, x1, yLo, dp + 0.05f);
    if (beam >= 0 && roof >= 0) v_edge(v, beam, roof, VE_SUPPORTS);
    F->roofTop = yHi; F->roofPart = roof; v->roofY = yLo - 0.12f;
    /* a counter at the front, or a trestle table */
    float cd = rng_range(r, 0.55f, 0.75f), ch = rng_range(r, 0.96f, 1.05f);
    float cz1 = dp - 0.08f, cz0 = cz1 - cd;
    int body = v_box(v, VR_SHELL, VS_FINISH, VCOL_ACC2, x0 + 0.15f, 0.0f, cz0 + 0.03f, x1 - 0.65f, ch - 0.04f, cz1 - 0.03f);
    int top = v_box(v, VR_COUNTER, VS_CHIPBOARD, VCOL_TRIM, x0 + 0.12f, ch - 0.04f, cz0, x1 - 0.62f, ch, cz1);
    if (body >= 0) v->part[body].faces = VB_BOT;
    if (body >= 0 && top >= 0) v_edge(v, body, top, VE_SUPPORTS);
    add_surface(F, VSU_COUNTER, x0 + 0.18f, cz0 + 0.05f, x1 - 0.7f, cz1 - 0.05f, ch);
    v->front = cz1;
    /* shelves against the wall, a painted room in them */
    float sd = minf(rng_range(r, 0.4f, 0.55f), cz0 - 0.08f - 0.8f);
    if (sd >= 0.3f) {
        float sx0 = x0 + 0.3f, sx1 = x1 - 0.3f, sh = minf(2.1f, yHi - 0.35f);
        int shelf = v_box(v, VR_CABINET, VS_CHIPBOARD, VCOL_TRIM, sx0, 0.0f, 0.08f, sx1, sh, 0.08f + sd);
        if (shelf >= 0) {
            v->part[shelf].faces = VB_BOT | VB_NZ;
            v_opening(v, VO_RECESS, sx0 + 0.08f, 0.35f, 0.08f + sd + 0.005f, 1.0f, 0.0f, sx1 - sx0 - 0.16f, sh - 0.45f, sd - 0.05f, shelf);
        }
        add_surface(F, VSU_SHELF, sx0 + 0.05f, 0.1f, sx1 - 0.05f, 0.06f + sd, sh);
    }
    /* side cloths from the sheet's edges */
    float pc = v->cls == VC_POOR ? 0.75f : v->cls == VC_MID ? 0.5f : 0.25f;
    for (int s = -1; s <= 1; s += 2) {
        if (rng_float(r) >= pc) continue;
        float x = s < 0 ? x0 + 0.01f : x1 - 0.01f, bot = rng_float(r) < 0.5f ? rng_range(r, 0.2f, 0.9f) : 0.05f;
        float cq[4][3] = { { x, yHi - 0.05f, 0.1f }, { x, yLo - 0.05f, dp - 0.1f }, { x, bot, dp - 0.1f }, { x, bot, 0.1f } };
        int c = v_quad(v, VR_CLOTH, rng_float(r) < 0.5f ? VS_TARP : VS_CLOTH, VCOL_ACC1, cq, VPF_TWOSIDED | VPF_SWAY);
        if (c >= 0 && roof >= 0) v_edge(v, c, roof, VE_ATTACHED_TO);
    }
    uint32_t lc = lamp_rgb(v);
    int nb = 1 + (lw > 3.5f);
    for (int k = 0; k < nb; ++k) {
        float x = x0 + lw * ((float)k + 0.5f) / (float)nb, y = yHi - fall * (dp * 0.55f) / dp - 0.35f;
        v_light(v, v->cls == VC_RICH ? VLK_LED : VLK_BULB, x, y, dp * 0.55f, 0.7f, lc);
        int b = v_box(v, VR_LAMP, VS_BULB, VCOL_RGB, x - 0.05f, y - 0.05f, dp * 0.55f - 0.05f, x + 0.05f, y + 0.05f, dp * 0.55f + 0.05f);
        if (b >= 0) { v->part[b].rgb = lc; v->part[b].flags |= VPF_EMISSIVE; }
    }
    if (V_TRADES[v->trade].cooking == 2) { F->cook[0] = x1 - 1.0f; F->cook[1] = ch; F->cook[2] = 0.5f * (cz0 + cz1); }
    add_rail(F, x0 + 0.15f, dp - 0.02f, x1 - 0.15f, dp - 0.02f, yLo - 0.14f, 1);
    add_rail(F, x0 + 0.3f, cz0 - 0.4f, x1 - 0.3f, cz0 - 0.4f, yHi - fall * (cz0 - 0.4f) / dp - 0.1f, 0);
    F->front = cz1;
    F->signY = 0.0f;
    /* a board along the sheet's front edge */
    v_sign(v, VSG_FASCIA, 0.0f, yF + 0.32f, zF + 0.03f, 0.0f, 1.0f, minf(lw - 0.4f, 3.2f), 0.45f, 0);
    if (W - lw > 0.8f) v_slot(v, VSL_GROUND, x1 + 0.25f * (W - lw), 0.0f, 0.5f * dp, 0.0f, 1.0f, 0.5f * (W - lw) - 0.15f, 1.8f);
    if (W - lw > 0.8f) v_slot(v, VSL_GROUND, x0 - 0.25f * (W - lw), 0.0f, 0.5f * dp, 0.0f, 1.0f, 0.5f * (W - lw) - 0.15f, 1.8f);
    v_slot(v, VSL_GROUND, x1 - 0.35f, 0.0f, 0.5f * (cz0 + cz1), 0.0f, 1.0f, 0.5f, 1.0f);
    v_slot(v, VSL_WALL, 0.0f, yHi + 0.4f, 0.0f, 0.0f, 1.0f, lw - 0.3f, 0.9f);
    v->box[0] = x0; v->box[1] = 0.0f; v->box[2] = x1; v->box[3] = zF;
    return 1;
}

/* ------------------------------------------------------- shipping containers --- */

/* A container's shell with openings in its long front wall (z = z1 if facing +z) or its side
 * (x = x0 facing -x). op: n openings, each from a to b along the wall, sill to head. */
typedef struct BoxOpen { float a, b, sill, head; int kind; } BoxOpen;

static void container_shell(Vendor *v, int ci, int facing, const BoxOpen *op, int n, int *frontPart, int *head) {
    float x0 = v->cbox[ci][0], z0 = v->cbox[ci][1], x1 = v->cbox[ci][2], z1 = v->cbox[ci][3], y0 = v->cbox[ci][4], y1 = v->cbox[ci][5];
    const float t = 0.05f;
    int surf = v->wear > 0.65f ? VS_RUST : VS_CORRUGATED;
    int col = VCOL_BODY;
    int fz = facing == 0;   /* openings in the +z wall; else in the -x wall */
    float yb = y0 + 0.15f, yt = y1 - t;
    /* floor and roof, and the walls without openings */
    int roof = v_box(v, VR_SHELL, surf, col, x0, yt, z0, x1, y1, z1);
    v_box(v, VR_SHELL, VS_STEEL, col, x0, y0, z0, x1, yb, z1);
    if (fz) {
        v_box(v, VR_SHELL, surf, col, x0, yb, z0, x1, yt, z0 + t);
        v_box(v, VR_SHELL, surf, col, x0, yb, z0 + t, x0 + t, yt, z1 - t);
        v_box(v, VR_SHELL, surf, col, x1 - t, yb, z0 + t, x1, yt, z1 - t);
    } else {
        v_box(v, VR_SHELL, surf, col, x0 + t, yb, z0, x1, yt, z0 + t);
        v_box(v, VR_SHELL, surf, col, x0 + t, yb, z1 - t, x1, yt, z1);
        v_box(v, VR_SHELL, surf, col, x1 - t, yb, z0 + t, x1, yt, z1 - t);
    }
    /* the front wall in pieces round its openings: corner posts, piers, sills and head rails */
    float L = fz ? x1 - x0 : z1 - z0, base = fz ? x0 : z0;
    float at = 0.0f;
    int fp = -1;
    for (int k = 0; k <= n; ++k) {
        float a = k < n ? op[k].a : L;
        if (a - at > 0.01f) {
            int p = fz ? v_box(v, VR_SHELL, surf, col, base + at, yb, z1 - t, base + a, yt, z1)
                       : v_box(v, VR_SHELL, surf, col, x0, yb, base + at, x0 + t, yt, base + a);
            if (fp < 0) fp = p;
        }
        if (k == n) break;
        float b = op[k].b;
        if (op[k].sill > yb + 0.01f) {
            if (fz) v_box(v, VR_SHELL, surf, col, base + a, yb, z1 - t, base + b, op[k].sill, z1);
            else    v_box(v, VR_SHELL, surf, col, x0, yb, base + a, x0 + t, op[k].sill, base + b);
        }
        int h = fz ? v_box(v, VR_SHELL, surf, col, base + a, op[k].head, z1 - t, base + b, yt, z1)
                   : v_box(v, VR_SHELL, surf, col, x0, op[k].head, base + a, x0 + t, yt, base + b);
        if (head) head[k] = h;
        at = b;
    }
    if (frontPart) *frontPart = fp >= 0 ? fp : roof;
    /* corner castings, a centimetre proud */
    for (int c = 0; c < 8; ++c) {
        float cx = (c & 1) ? x1 - 0.09f : x0 + 0.09f, cz = (c & 2) ? z1 - 0.09f : z0 + 0.09f, cy = (c & 4) ? y1 - 0.09f : y0 + 0.09f;
        int p = v_box(v, VR_SHELL, VS_STEEL, VCOL_TRIM, cx - 0.1f, cy - 0.09f, cz - 0.1f, cx + 0.1f, cy + 0.09f, cz + 0.1f);
        if (p >= 0) v->part[p].faces = VB_BOT;
    }
    /* the doors' locking bars on the far end: +x for a container along x, +z for one along z */
    for (int k = 0; k < 4; ++k) {
        float o = 0.14f + 0.24f * (float)k;
        float bx = fz ? x1 + 0.03f : x0 + (x1 - x0) * o, bz = fz ? z0 + (z1 - z0) * o : z1 + 0.03f;
        v_cyl(v, VR_SHELL, VS_STEEL, VCOL_TRIM, bx, y0 + 0.25f, bz, bx, y1 - 0.25f, bz, 0.02f, 4);
    }
}

/* Plans n openings along a wall of length L with corner posts, piers and the share rule. */
static int plan_openings(Rng *r, float L, int n, int walkin, float y0, float y1, BoxOpen *op) {
    int shortWall = L < 4.0f;   /* a 10 ft box's side: as much opening as the posts allow */
    float cp = shortWall ? rng_range(r, 0.62f, 0.7f) : rng_range(r, 0.65f, 0.95f), pier = rng_range(r, 0.5f, 0.8f);
    float avail = L - 2.0f * cp - (float)(n - 1) * pier;
    float total = minf(avail, V_OPENING_SHARE * L - 0.02f) * (shortWall ? rng_range(r, 0.9f, 1.0f) : rng_range(r, 0.75f, 1.0f));
    float each = minf(total / (float)n, walkin ? 4.0f : 3.0f);
    if (each < 0.8f) return n > 1 ? plan_openings(r, L, n - 1, walkin, y0, y1, op) : 0;   /* fewer, wider openings */
    total = each * (float)n;
    float spare = avail - total, gap = spare / (float)(n + 1);
    float at = cp + gap;
    float floorY = y0 + 0.15f;
    for (int k = 0; k < n; ++k) {
        op[k].a = at; op[k].b = at + each;
        op[k].kind = walkin ? VO_WALKIN : VO_HATCH;
        op[k].sill = walkin ? floorY : floorY + rng_range(r, 0.95f, 1.05f);
        op[k].head = y1 - rng_range(r, 0.33f, 0.42f);
        at += each + pier + gap;
    }
    return n;
}

/* An opening's room, and what covers it: a flap on struts or a cloth awning on posts, hung from its head rail. */
static void opening_cover(VGen *G, VFrame *F, int ci, const BoxOpen *o, int front, float outZ, float roomDepth, int awningOK) {
    Vendor *v = G->v;
    Rng *r = &G->rs;
    float x0 = v->cbox[ci][0], z1 = v->cbox[ci][3], y1 = v->cbox[ci][5];
    float a = x0 + o->a, b = x0 + o->b, hh = o->head - o->sill;
    v_opening(v, o->kind, a, o->sill, z1 + 0.002f, 1.0f, 0.0f, b - a, hh, roomDepth, front);
    v->open[v->nopen - 1].cbox = (uint8_t)ci;
    int flap = o->kind == VO_HATCH && hh <= V_FLAP_MAX && (!awningOK || rng_float(r) < 0.6f);
    if (flap) {
        float ang = rng_range(r, 1.0f, 1.3f);
        float q[4][3] = { { a, o->head, z1 + 0.01f }, { b, o->head, z1 + 0.01f },
                          { b, o->head + hh * cosf(ang), z1 + 0.01f + hh * sinf(ang) }, { a, o->head + hh * cosf(ang), z1 + 0.01f + hh * sinf(ang) } };
        int fl = v_quad(v, VR_FLAP, VS_CORRUGATED, VCOL_BODY, q, VPF_TWOSIDED);
        if (fl >= 0 && front >= 0) v_edge(v, fl, front, VE_ATTACHED_TO);
        for (int s = 0; s < 2; ++s) {
            float xs = s ? b - 0.04f : a + 0.04f;
            int st = v_beam(v, VR_STRUT, VS_STEEL, VCOL_TRIM, xs, o->head - 0.4f, z1 + 0.02f, xs, q[2][1] - 0.02f, q[2][2] - 0.03f, 0.025f);
            if (st >= 0 && fl >= 0) v_edge(v, st, fl, VE_SUPPORTS);
        }
    } else if (awningOK && outZ > 0.9f) {
        /* a cloth awning from over the opening on two posts */
        float ad = minf(outZ - 0.1f, rng_range(r, 1.2f, V_AWNING_MAX - 0.05f));
        float yw = y1 - 0.08f, yf = maxf(yw - rng_range(r, 0.28f, 0.4f), V_CLEAR_MIN + 0.02f);
        if (yw - yf < V_FALL_MIN) yf = yw - V_FALL_MIN;
        float q[4][3] = { { a - 0.15f, yw, z1 + 0.02f }, { b + 0.15f, yw, z1 + 0.02f }, { b + 0.15f, yf, z1 + ad }, { a - 0.15f, yf, z1 + ad } };
        int aw = v_quad(v, VR_AWNING, v->cls == VC_RICH ? VS_CLOTH : VS_STRIPE, v->cls == VC_RICH ? VCOL_TRIM : VCOL_ACC1, q, VPF_TWOSIDED);
        if (aw >= 0 && front >= 0) v_edge(v, front, aw, VE_SUPPORTS);
        for (int s = 0; s < 2; ++s) {
            int p = post(v, VS_STEEL, VCOL_TRIM, s ? b + 0.1f : a - 0.1f, z1 + ad - 0.05f, 0.0f, yf, 0.03f, 1);
            if (p >= 0 && aw >= 0) v_edge(v, p, aw, VE_SUPPORTS);
        }
        if (v->cls != VC_RICH && yf - 0.18f >= V_CLEAR_MIN + 0.02f) {
            float vq[4][3] = { { a - 0.15f, yf, z1 + ad }, { b + 0.15f, yf, z1 + ad }, { b + 0.15f, yf - 0.18f, z1 + ad }, { a - 0.15f, yf - 0.18f, z1 + ad } };
            int val = v_quad(v, VR_CLOTH, VS_STRIPE, VCOL_ACC1, vq, VPF_TWOSIDED | VPF_SCALLOP | VPF_SWAY);
            if (val >= 0 && aw >= 0) v_edge(v, val, aw, VE_ATTACHED_TO);
        }
        add_rail(F, a + 0.1f, z1 + ad - 0.15f, b - 0.1f, z1 + ad - 0.15f, yf - 0.05f, 1);
    }
    if (o->kind == VO_HATCH) {
        v_box(v, VR_SHELL, VS_LAMINATE, VCOL_TRIM, a - 0.04f, o->sill - 0.04f, z1, b + 0.04f, o->sill, z1 + 0.3f);
        add_surface(F, VSU_SHELF, a, z1 + 0.03f, b, z1 + 0.27f, o->sill);
    }
    uint32_t lc = lamp_rgb(v);
    v_light(v, lamp_kind(v), 0.5f * (a + b), o->head - 0.15f, z1 - 0.6f, 0.9f, lc);
}

static int build_container(VGen *G, VFrame *F) {
    Vendor *v = G->v;
    Rng *r = &G->rs;
    float W = v->site.w, D = v->site.d, ceil = ceiling_of(v);
    float L;
    float p40 = v->cls == VC_RICH ? 0.5f : v->cls == VC_MID ? 0.35f : 0.2f;
    if (W >= V_BOX40 + 0.3f && rng_float(r) < p40) L = V_BOX40;
    else if (W >= V_BOX20 + 0.2f) L = V_BOX20;
    else return 0;
    float fl = rng_range(r, 0.18f, 0.3f);
    if (fl + V_BOX_H + 0.1f > ceil || D < V_BOX_W + 1.2f) return 0;
    float zb = rng_range(r, 0.05f, 0.25f);
    float walkP = v->cls == VC_RICH ? 0.75f : v->cls == VC_MID ? 0.45f : 0.15f;
    if (V_TRADES[v->trade].cooking) walkP *= 0.35f;
    int walkin = rng_float(r) < walkP;
    float outZ = D - (zb + V_BOX_W);
    if (walkin && outZ < 1.3f) walkin = 0;
    int n = L > 10.0f ? rng_int(r, 2, 3) : (rng_float(r) < 0.3f ? 2 : 1);
    v->ncbox = 1;
    v->cboxAxis[0] = 0;
    float *c = v->cbox[0];
    c[0] = -0.5f * L; c[1] = zb; c[2] = 0.5f * L; c[3] = zb + V_BOX_W; c[4] = fl; c[5] = fl + V_BOX_H;
    BoxOpen op[3];
    n = plan_openings(r, L, n, walkin, fl, c[5], op);
    if (!n) return 0;
    v->layout = (L > 10.0f) * 8 + walkin * 4 + n;
    v->clearMin = V_CLEAR_MIN;
    int front = -1, heads[3] = { -1, -1, -1 };
    container_shell(v, 0, 0, op, n, &front, heads);
    /* sleepers under it */
    for (int s = 0; s < 2; ++s) {
        float x = s ? c[2] - 0.6f : c[0] + 0.6f;
        v_box(v, VR_SUPPORT, VS_TIMBER, VCOL_TRIM, x - 0.12f, 0.0f, zb, x + 0.12f, fl, c[3]);
    }
    float dd = 0.0f;
    if (walkin) {
        /* a deck at the container's floor, across the openings, and a step if it stands high */
        dd = minf(outZ - 0.4f, rng_range(r, 1.0f, 1.8f));
        float fy = fl + 0.15f, dx0 = c[0] + op[0].a - 0.4f, dx1 = c[0] + op[n - 1].b + 0.4f;
        int deck = v_box(v, VR_DECK, VS_DECK, VCOL_TRIM, dx0, fy - 0.08f, c[3], dx1, fy, c[3] + dd);
        if (deck >= 0) v->part[deck].faces = VB_BOT;
        if (fy > 0.2f) {
            int st = v_box(v, VR_STEP, VS_DECK, VCOL_TRIM, dx0 + 0.3f * (dx1 - dx0), 0.0f, c[3] + dd, dx0 + 0.7f * (dx1 - dx0), 0.5f * fy, c[3] + dd + 0.32f);
            if (st >= 0) v->part[st].faces = VB_BOT;
        }
    }
    int awnings = !(walkin == 0 && rng_float(r) < 0.35f);
    for (int k = 0; k < n; ++k) opening_cover(G, F, 0, &op[k], heads[k] >= 0 ? heads[k] : front, outZ, V_BOX_W - 0.12f, awnings);
    v->front = walkin ? c[3] + dd : c[3] + 0.3f;
    F->front = v->front;
    F->roofTop = c[5]; F->roofPart = front;
    if (V_TRADES[v->trade].cooking == 2) {
        F->cook[0] = c[0] + 0.5f * (op[0].a + op[0].b); F->cook[1] = op[0].sill; F->cook[2] = c[3] - 0.6f;
        float fx = c[2] - 0.8f, fz = c[1] + 0.6f;
        v_cyl(v, VR_FLUE, VS_STEEL, VCOL_TRIM, fx, c[5], fz, fx, minf(c[5] + rng_range(r, 0.65f, 1.0f), ceil - 0.05f), fz, 0.1f, 8);
    }
    F->signY = c[5]; F->signW = minf(L * 0.45f, 2.6f);
    roof_slots(v, c[0] + 0.4f, c[2] - 0.4f, c[1] + 0.2f, c[3] - 0.2f, c[5], L > 10.0f ? 4 : 2);
    v_slot(v, VSL_WALL, c[0] - 0.005f, 1.5f, 0.5f * (c[1] + c[3]), -1.0f, 0.0f, V_BOX_W - 0.4f, 1.6f);
    v_slot(v, VSL_WALL, c[2] + 0.005f, 1.5f, 0.5f * (c[1] + c[3]), 1.0f, 0.0f, V_BOX_W - 0.4f, 1.6f);
    if (op[0].a > 0.95f) v_slot(v, VSL_WALL, c[0] + 0.5f * op[0].a, fl + 1.5f, c[3] + 0.005f, 0.0f, 1.0f, op[0].a - 0.3f, 1.4f);
    if (W - L > 1.0f) {
        v_slot(v, VSL_GROUND, c[0] - 0.25f * (W - L), 0.0f, 0.5f * (c[1] + c[3]), 0.0f, 1.0f, 0.5f * (W - L) - 0.15f, 1.6f);
        v_slot(v, VSL_GROUND, c[2] + 0.25f * (W - L), 0.0f, 0.5f * (c[1] + c[3]), 0.0f, 1.0f, 0.5f * (W - L) - 0.15f, 1.6f);
    }
    if (walkin) v_slot(v, VSL_GROUND, c[0] + op[0].a - 0.2f, fl + 0.15f, c[3] + 0.4f * dd, 0.0f, 1.0f, 0.55f, 1.2f);
    v->box[0] = c[0]; v->box[1] = 0.0f; v->box[2] = c[2]; v->box[3] = c[3] + maxf(dd + (walkin && fl + 0.15f > 0.2f ? 0.32f : 0.0f), 0.3f);
    for (int k = 0; k < v->npart; ++k) if (v->part[k].role == VR_AWNING) v->box[3] = maxf(v->box[3], v->part[k].q[2][2]);
    return 1;
}

static int build_pair(VGen *G, VFrame *F) {
    Vendor *v = G->v;
    Rng *r = &G->rs;
    float W = v->site.w, D = v->site.d, ceil = ceiling_of(v);
    float fl = rng_range(r, 0.18f, 0.28f);
    if (W < V_BOX20 + 0.2f || fl + V_BOX_H + 0.1f > ceil) return 0;
    float L = W >= V_BOX40 + 0.3f && rng_float(r) < 0.4f ? V_BOX40 : V_BOX20;
    float zb = 0.1f, room = D - (zb + V_BOX_W) - 0.1f;
    float L2 = room >= V_BOX20 + 0.2f && rng_float(r) < 0.5f ? V_BOX20 : V_BOX10;
    if (room < L2) { L2 = V_BOX10; if (room < L2) return 0; }
    float cw = rng_range(r, 3.0f, minf(V_COURT_MAX - 0.05f, L - V_BOX_W - 0.2f));
    if (cw < 2.6f) return 0;
    float *a = v->cbox[0], *b = v->cbox[1];
    a[0] = -0.5f * L; a[1] = zb; a[2] = 0.5f * L; a[3] = zb + V_BOX_W; a[4] = fl; a[5] = fl + V_BOX_H;
    float xs = a[0] + cw;
    b[0] = xs; b[1] = a[3] + 0.02f; b[2] = xs + V_BOX_W; b[3] = b[1] + L2; b[4] = fl; b[5] = fl + V_BOX_H;
    v->ncbox = 2; v->cboxAxis[0] = 0; v->cboxAxis[1] = 1;
    v->court = cw;
    v->layout = (L > 10.0f) * 4 + (L2 > 4.0f) * 2;
    v->clearMin = V_CLEAR_MIN;
    /* the main container's openings face the court; the second's its -x side */
    BoxOpen op[2], op2[1];
    int n = cw > 3.6f && rng_float(r) < 0.5f ? 2 : 1;
    n = plan_openings(r, cw, n, 0, fl, a[5], op);
    if (!n) return 0;
    for (int k = 0; k < n; ++k) op[k].kind = rng_float(r) < (v->cls == VC_POOR ? 0.2f : 0.5f) ? VO_WALKIN : VO_HATCH;
    for (int k = 0; k < n; ++k) if (op[k].kind == VO_WALKIN) op[k].sill = fl + 0.15f;
    if (!plan_openings(r, L2, 1, 0, fl, b[5], op2)) return 0;
    int f0 = -1, f1 = -1;
    container_shell(v, 0, 0, op, n, &f0, NULL);
    container_shell(v, 1, 1, op2, 1, &f1, NULL);
    for (int k = 0; k < n; ++k) {
        float oa = a[0] + op[k].a, ob = a[0] + op[k].b;
        v_opening(v, op[k].kind, oa, op[k].sill, a[3] + 0.002f, 1.0f, 0.0f, ob - oa, op[k].head - op[k].sill, V_BOX_W - 0.12f, f0);
        v->open[v->nopen - 1].cbox = 0;
        if (op[k].kind == VO_HATCH) {
            v_box(v, VR_SHELL, VS_LAMINATE, VCOL_TRIM, oa - 0.04f, op[k].sill - 0.04f, a[3], ob + 0.04f, op[k].sill, a[3] + 0.3f);
            add_surface(F, VSU_SHELF, oa, a[3] + 0.03f, ob, a[3] + 0.27f, op[k].sill);
        }
    }
    {
        float za = b[1] + op2[0].a, zb2 = b[1] + op2[0].b;
        /* seen from the court (-x), left to right runs towards +z */
        v_opening(v, op2[0].kind, b[0] - 0.002f, op2[0].sill, za, 0.0f, 1.0f, zb2 - za, op2[0].head - op2[0].sill, V_BOX_W - 0.12f, f1);
        v->open[v->nopen - 1].cbox = 1;
        v_box(v, VR_SHELL, VS_LAMINATE, VCOL_TRIM, b[0] - 0.3f, op2[0].sill - 0.04f, za - 0.04f, b[0], op2[0].sill, zb2 + 0.04f);
        add_surface(F, VSU_SHELF, b[0] - 0.27f, za, b[0] - 0.03f, zb2, op2[0].sill);
    }
    for (int s = 0; s < 2; ++s)
        for (int k = 0; k < 2; ++k) {
            float *c = v->cbox[s];
            if (s == 0) { float x = k ? c[2] - 0.6f : c[0] + 0.6f; v_box(v, VR_SUPPORT, VS_TIMBER, VCOL_TRIM, x - 0.12f, 0.0f, c[1], x + 0.12f, fl, c[3]); }
            else        { float z = k ? c[3] - 0.6f : c[1] + 0.6f; v_box(v, VR_SUPPORT, VS_TIMBER, VCOL_TRIM, c[0], 0.0f, z - 0.12f, c[2], fl, z + 0.12f); }
        }
    /* the tarp over the court, from the main container's top down to the front, and its posts */
    float yw = a[5] - 0.04f, fall = minf(rng_range(r, 0.27f, 0.42f), yw - V_CLEAR_MIN - 0.02f);
    if (fall < V_FALL_MIN) return 0;
    float zF = b[3];
    float q[4][3] = { { a[0], yw, a[3] }, { xs, yw, a[3] }, { xs, yw - fall, zF }, { a[0], yw - fall, zF } };
    int tarp = v_quad(v, VR_ROOF, VS_TARP, VCOL_ACC1, q, VPF_TWOSIDED);
    if (tarp >= 0) { if (f0 >= 0) v_edge(v, f0, tarp, VE_SUPPORTS); if (f1 >= 0) v_edge(v, f1, tarp, VE_SUPPORTS); }
    int pz = (int)ceilf((zF - a[3]) / (V_SPAN_MAX - 0.05f));
    for (int k = 1; k <= pz; ++k) {
        float z = a[3] + (zF - a[3]) * (float)k / (float)pz;
        int p = post(v, VS_STEEL, VCOL_TRIM, a[0] + 0.05f, z - (k == pz ? 0.05f : 0.0f), 0.0f, yw - fall * (z - a[3]) / (zF - a[3]), 0.04f, 1);
        if (p >= 0 && tarp >= 0) v_edge(v, p, tarp, VE_SUPPORTS);
    }
    v->roofY = yw - fall;
    F->roofTop = a[5]; F->roofPart = f0;
    /* the court: a high table and stools round it */
    float tx = a[0] + 0.5f * cw, tz = a[3] + 0.5f * (zF - a[3]) + 0.3f;
    v_cyl(v, VR_SHELL, VS_STEEL, VCOL_TRIM, tx, 0.0f, tz, tx, 1.02f, tz, 0.05f, 6);
    int tt = v_cyl(v, VR_SHELL, VS_CHIPBOARD, VCOL_TRIM, tx, 1.0f, tz, tx, 1.05f, tz, 0.4f, 10);
    (void)tt;
    add_surface(F, VSU_SHELF, tx - 0.25f, tz - 0.25f, tx + 0.25f, tz + 0.25f, 1.05f);
    int ns = 0;
    for (int k = 0; k < 4; ++k) {
        float ang = 0.5f * PI_F * (float)k + 0.4f;
        VItem *it = v_item(v, VI_STOOL, VR_STOOL);
        if (!it) break;
        it->p[0] = tx + 0.72f * cosf(ang); it->p[1] = 0.0f; it->p[2] = tz + 0.72f * sinf(ang);
        it->s[0] = 0.36f; it->s[1] = 0.74f; it->s[2] = 0.36f; it->fx = -cosf(ang); it->fz = -sinf(ang); it->seed = rng_u32(r);
        ns++;
    }
    v->st.stools = ns;
    uint32_t lc = lamp_rgb(v);
    /* a string of bulbs across the court, the containers lit inside */
    for (int k = 0; k < 3; ++k) {
        float x = a[0] + cw * (0.25f + 0.25f * (float)k), z = a[3] + 0.5f * (zF - a[3]), y = yw - fall * 0.5f - 0.2f;
        int bl = v_box(v, VR_LAMP, VS_BULB, VCOL_RGB, x - 0.05f, y - 0.05f, z - 0.05f, x + 0.05f, y + 0.05f, z + 0.05f);
        if (bl >= 0) { v->part[bl].rgb = 0xFFD8A0u; v->part[bl].flags |= VPF_EMISSIVE; }
        v_light(v, VLK_BULB, x, y, z, 0.35f, 0xFFD8A0u);
    }
    v_light(v, lamp_kind(v), a[0] + 0.5f * cw, a[5] - 0.5f, a[3] - 0.7f, 0.9f, lc);
    v_light(v, lamp_kind(v), b[0] + 0.7f, b[5] - 0.5f, 0.5f * (b[1] + b[3]), 0.9f, lc);
    add_rail(F, a[0] + 0.3f, a[3] + 0.3f, xs - 0.3f, a[3] + 0.3f, yw - 0.1f, 1);
    if (V_TRADES[v->trade].cooking == 2) {
        F->cook[0] = a[0] + 0.5f * cw; F->cook[1] = op[0].sill; F->cook[2] = a[3] - 0.6f;
        v_cyl(v, VR_FLUE, VS_STEEL, VCOL_TRIM, a[2] - 0.7f, a[5], a[1] + 0.6f, a[2] - 0.7f, minf(a[5] + rng_range(r, 0.65f, 1.0f), ceil - 0.05f), a[1] + 0.6f, 0.1f, 8);
    }
    v->front = a[3];
    F->front = a[3];
    F->signY = a[5]; F->signW = minf(cw, 2.6f);
    roof_slots(v, a[0] + 0.4f, a[2] - 0.4f, a[1] + 0.2f, a[3] - 0.2f, a[5], L > 10.0f ? 4 : 2);
    roof_slots(v, b[0] + 0.3f, b[2] - 0.3f, b[1] + 0.5f, b[3] - 0.5f, b[5], 1);
    v_slot(v, VSL_WALL, a[0] - 0.005f, 1.5f, 0.5f * (a[1] + a[3]), -1.0f, 0.0f, V_BOX_W - 0.4f, 1.6f);
    v_slot(v, VSL_WALL, b[2] + 0.005f, 1.5f, 0.5f * (b[1] + b[3]), 1.0f, 0.0f, L2 - 0.4f, 1.6f);
    v_slot(v, VSL_GROUND, a[0] + 0.45f, 0.0f, zF - 0.4f, 0.0f, 1.0f, 0.6f, 1.4f);
    v_slot(v, VSL_GROUND, xs - 0.4f, 0.0f, a[3] + 0.45f, 0.0f, 1.0f, 0.55f, 1.4f);
    v->box[0] = a[0]; v->box[1] = 0.0f; v->box[2] = a[2]; v->box[3] = zF;
    return 1;
}

/* ------------------------------------------------------------ market stall --- */

static int build_market(VGen *G, VFrame *F) {
    Vendor *v = G->v;
    Rng *r = &G->rs;
    float W = v->site.w, D = v->site.d, ceil = ceiling_of(v);
    if (W < 2.1f || D < 2.3f || ceil < 2.9f) return 0;
    float sw = minf(W - 0.1f, rng_range(r, 2.2f, 5.5f)), sd = minf(D - 0.6f, rng_range(r, 1.8f, 2.8f));
    if (sd < 1.6f) return 0;
    float x0 = -0.5f * sw, x1 = 0.5f * sw;
    float w3[3] = { 0.3f, 0.5f, 0.2f };   /* stripe, tarp, parasol */
    if (v->cls == VC_RICH) { w3[0] = 0.6f; w3[1] = 0.1f; w3[2] = 0.3f; }
    else if (v->cls == VC_MID) { w3[0] = 0.5f; w3[1] = 0.3f; w3[2] = 0.2f; }
    int kind = v_pick(r, w3, 3);
    float th = rng_range(r, 0.82f, 0.95f), td = rng_range(r, 0.75f, 1.05f);
    float tz1 = sd, tz0 = sd - td;
    /* a parasol stands behind the table: if the site can't take one wide enough, a tarp instead */
    float ppz = maxf(tz0 - 0.25f, 0.3f), pR = minf(minf(V_PARASOL_R, 0.5f * W - 0.05f), minf(D - ppz - 0.05f, ppz - 0.02f));
    if (kind == 2 && pR < 1.0f) kind = 1;
    v->variant = kind;
    int skirt = rng_float(r) < 0.6f;
    int tiers = 1 + (rng_float(r) < 0.6f) + (rng_float(r) < 0.35f);
    v->layout = kind * 8 + tiers * 2 + skirt;
    v->front = tz1;
    v->clearMin = V_MARKET_LIP;
    /* the table: a top, skirted or on trestles, and tiers rising to the back */
    int table = v_box(v, VR_TABLE, VS_CHIPBOARD, VCOL_TRIM, x0 + 0.1f, th - 0.04f, tz0, x1 - 0.1f, th, tz1);
    if (skirt) {
        float q[4][3] = { { x0 + 0.1f, th - 0.02f, tz1 + 0.01f }, { x1 - 0.1f, th - 0.02f, tz1 + 0.01f }, { x1 - 0.1f, 0.04f, tz1 + 0.01f }, { x0 + 0.1f, 0.04f, tz1 + 0.01f } };
        int sk = v_quad(v, VR_CLOTH, VS_CLOTH, VCOL_ACC2, q, VPF_SWAY);
        if (sk >= 0 && table >= 0) v_edge(v, sk, table, VE_ATTACHED_TO);
        /* four square legs at its corners, the front two behind the skirt */
        for (int c = 0; c < 4; ++c) {
            float x = c & 1 ? x1 - 0.18f : x0 + 0.13f, z = c & 2 ? tz1 - 0.08f : tz0 + 0.03f;
            int leg = v_box(v, VR_SUPPORT, VS_TIMBER, VCOL_TRIM, x, 0.0f, z, x + 0.05f, th - 0.04f, z + 0.05f);
            if (leg >= 0) { v->part[leg].faces = VB_BOT | VB_TOP; if (table >= 0) v_edge(v, leg, table, VE_SUPPORTS); }
        }
    } else {
        for (int s = 0; s < 2; ++s) {
            float x = s ? x1 - 0.25f : x0 + 0.25f;
            int l1 = v_beam(v, VR_SUPPORT, VS_TIMBER, VCOL_TRIM, x, 0.0f, tz0 + 0.1f, x, th - 0.04f, tz1 - 0.1f, 0.04f);
            int l2 = v_beam(v, VR_SUPPORT, VS_TIMBER, VCOL_TRIM, x, 0.0f, tz1 - 0.1f, x, th - 0.04f, tz0 + 0.1f, 0.04f);
            if (table >= 0) { if (l1 >= 0) v_edge(v, l1, table, VE_SUPPORTS); if (l2 >= 0) v_edge(v, l2, table, VE_SUPPORTS); }
        }
    }
    float tierD = td / ((float)tiers + 0.4f);
    add_surface(F, VSU_TABLE, x0 + 0.15f, tz1 - tierD * 1.4f + 0.03f, x1 - 0.15f, tz1 - 0.04f, th);
    for (int k = 1; k < tiers; ++k) {
        float zk1 = tz1 - tierD * (1.4f + (float)(k - 1)), yk = th + 0.13f * (float)k;
        /* each tier stands on the one below, so no two share a face */
        int tp = v_box(v, VR_TIER, VS_CHIPBOARD, VCOL_TRIM, x0 + 0.12f, yk - 0.13f, tz0, x1 - 0.12f, yk, zk1);
        if (tp >= 0) v->part[tp].faces = VB_BOT;
        if (tp >= 0 && table >= 0) v_edge(v, table, tp, VE_SUPPORTS);
        add_surface(F, VSU_TIER, x0 + 0.15f, zk1 - tierD + 0.03f, x1 - 0.15f, zk1 - 0.03f, yk);
    }
    /* the back: a cloth closing the stall, the painted back room behind it */
    float rd = minf(rng_range(r, 0.45f, 0.9f), tz0 - 0.7f);
    float yb, yf, zF;
    int canopy = -1;
    if (kind != 2) {
        /* a plane from the back (yb at z = 0) to its lip (ycf at zF), the lip (and the stripe's
         * valance under it) clear of the market's minimum */
        float val = kind == 0 ? 0.2f : 0.0f;
        float ov = minf(rng_range(r, 0.2f, 0.6f), D - sd - 0.05f);
        zF = sd + ov;
        float ycf = V_MARKET_LIP + 0.03f + val + rng_range(r, 0.0f, 0.2f);
        yb = minf(ycf + rng_range(r, 0.3f, 0.5f), ceil - 0.15f);
        if (yb - ycf < V_FALL_MIN + 0.01f) return 0;
        yf = yb - (yb - ycf) * (sd - 0.05f) / zF;   /* at the front poles */
        float q[4][3] = { { x0 - 0.05f, yb, 0.0f }, { x1 + 0.05f, yb, 0.0f }, { x1 + 0.05f, ycf, zF }, { x0 - 0.05f, ycf, zF } };
        canopy = v_quad(v, VR_ROOF, kind == 0 ? VS_STRIPE : VS_TARP, kind == 0 ? VCOL_ACC1 : VCOL_BODY, q, VPF_TWOSIDED);
        post_row(v, VS_STEEL, VCOL_TRIM, x0 + 0.04f, x1 - 0.04f, 0.05f, yb, 0.025f, 1, canopy, NULL, 0);
        post_row(v, VS_STEEL, VCOL_TRIM, x0 + 0.04f, x1 - 0.04f, sd - 0.05f, yf, 0.025f, 1, canopy, NULL, 0);
        if (kind == 0) {
            float vq[4][3] = { { x0 - 0.05f, ycf, zF }, { x1 + 0.05f, ycf, zF }, { x1 + 0.05f, ycf - 0.2f, zF }, { x0 - 0.05f, ycf - 0.2f, zF } };
            int val = v_quad(v, VR_CLOTH, VS_STRIPE, VCOL_ACC1, vq, VPF_TWOSIDED | VPF_SCALLOP | VPF_SWAY);
            if (val >= 0 && canopy >= 0) v_edge(v, val, canopy, VE_ATTACHED_TO);
            if (val >= 0) v_sign(v, VSG_VALANCE, 0.0f, ycf - 0.1f, zF + 0.005f, 0.0f, 1.0f, sw * 0.6f, 0.16f, 0);
        }
        F->roofTop = yb; F->roofPart = canopy; v->roofY = ycf - (kind == 0 ? 0.2f : 0.0f);
        add_rail(F, x0 + 0.2f, zF - 0.12f, x1 - 0.2f, zF - 0.12f, ycf - 0.06f - (kind == 0 ? 0.2f : 0.0f), 1);
        /* side cloths, sometimes */
        if (rng_float(r) < (v->cls == VC_RICH ? 0.2f : 0.45f)) {
            for (int s = -1; s <= 1; s += 2) {
                float x = s < 0 ? x0 + 0.02f : x1 - 0.02f;
                float cq[4][3] = { { x, yb - 0.05f, 0.08f }, { x, yf - 0.05f, sd - 0.12f }, { x, th + 0.3f, sd - 0.12f }, { x, th + 0.3f, 0.08f } };
                v_quad(v, VR_CLOTH, VS_CLOTH, VCOL_ACC2, cq, VPF_TWOSIDED | VPF_SWAY);
            }
        }
    } else {
        /* one big parasol over the table, its pole behind it */
        float pz = maxf(tz0 - 0.25f, 0.3f);
        float R = minf(minf(V_PARASOL_R, 0.5f * W - 0.05f), minf(D - pz - 0.05f, pz - 0.02f));
        if (R < 1.0f) return 0;
        float rim = rng_range(r, 2.05f, 2.3f), apex = rim + rng_range(r, 0.35f, 0.5f);
        if (apex > ceil - 0.1f) return 0;
        int pole = v_cyl(v, VR_SUPPORT, VS_CHROME, VCOL_TRIM, 0.0f, 0.0f, pz, 0.0f, apex, pz, 0.025f, 6);
        canopy = v_part(v, VP_CONE, VR_ROOF, VS_STRIPE, VCOL_ACC1, VL_SHELL);
        if (canopy >= 0) {
            VPart *p = &v->part[canopy];
            p->a[0] = 0.0f; p->a[1] = apex; p->a[2] = pz; p->b[0] = 0.0f; p->b[1] = rim; p->b[2] = pz; p->r = R; p->segs = 12; p->flags |= VPF_TWOSIDED;
        }
        if (pole >= 0 && canopy >= 0) v_edge(v, pole, canopy, VE_SUPPORTS);
        yb = apex; yf = rim; zF = pz + R;
        F->roofTop = apex; F->roofPart = canopy; v->roofY = rim;
        v->clearMin = V_PARASOL_RIM;
        rd = minf(rd, pz - 0.35f);
    }
    if (rd >= 0.35f) {
        float q[4][3] = { { x0 + 0.05f, 0.02f, rd }, { x1 - 0.05f, 0.02f, rd }, { x1 - 0.05f, minf(yb - 0.08f, 2.2f), rd }, { x0 + 0.05f, minf(yb - 0.08f, 2.2f), rd } };
        int back = v_quad(v, VR_CLOTH, VS_CLOTH, VCOL_ACC2, q, VPF_TWOSIDED | VPF_SWAY);
        if (back >= 0) v_opening(v, VO_BACKCLOTH, x0 + 0.1f, 0.6f, rd + 0.004f, 1.0f, 0.0f, sw - 0.2f, minf(yb - 0.08f, 2.2f) - 0.65f, rd - 0.04f, back);
    }
    if (V_TRADES[v->trade].cooking == 2) {
        float sx = x1 - 0.5f, szz = maxf(tz0 - 0.3f, 0.25f);
        F->cook[0] = sx; F->cook[1] = th; F->cook[2] = szz;
        v_box(v, VR_SHELL, VS_STEEL, VCOL_TRIM, sx - 0.25f, 0.0f, szz - 0.22f, sx + 0.25f, th, szz + 0.22f);
        int pipe = v_cyl(v, VR_FLUE, VS_STEEL, VCOL_TRIM, sx, th, szz, sx, minf(yb + rng_range(r, 0.65f, 0.9f), ceil - 0.05f), szz, 0.06f, 6);
        if (pipe >= 0 && canopy >= 0) v_edge(v, pipe, canopy, VE_ATTACHED_TO);
    }
    uint32_t lc = lamp_rgb(v);
    int nb = 1 + (sw > 3.2f);
    for (int k = 0; k < nb; ++k) {
        float x = x0 + sw * ((float)k + 0.5f) / (float)nb, z = 0.5f * (tz0 + tz1), y = kind == 2 ? yf - 0.15f : yf + (yb - yf) * (1.0f - z / sd) - 0.3f;
        int b = v_box(v, VR_LAMP, VS_BULB, VCOL_RGB, x - 0.05f, y - 0.05f, z - 0.05f, x + 0.05f, y + 0.05f, z + 0.05f);
        if (b >= 0) { v->part[b].rgb = lc; v->part[b].flags |= VPF_EMISSIVE; }
        v_light(v, v->cls == VC_RICH ? VLK_LED : VLK_BULB, x, y, z, 0.6f, lc);
    }
    v_slot(v, VSL_GROUND, x0 + 0.45f, 0.0f, 0.5f * (tz0 + tz1), 0.0f, 1.0f, 0.55f, th - 0.1f);
    v_slot(v, VSL_GROUND, x1 - 0.45f, 0.0f, 0.5f * (tz0 + tz1), 0.0f, 1.0f, 0.55f, th - 0.1f);
    if (W - sw > 0.8f) v_slot(v, VSL_GROUND, x1 + 0.25f * (W - sw), 0.0f, 0.5f * sd, 0.0f, 1.0f, 0.5f * (W - sw) - 0.15f, 1.6f);
    F->front = tz1;
    F->signY = 0.0f;
    v->box[0] = kind == 2 ? -maxf(0.5f * sw, 0.0f) : x0 - 0.05f; v->box[1] = 0.0f; v->box[2] = kind == 2 ? 0.5f * sw : x1 + 0.05f; v->box[3] = maxf(zF, tz1);
    return 1;
}

/* ------------------------------------------------------------ hand barrow --- */

static int build_barrow(VGen *G, VFrame *F) {
    Vendor *v = G->v;
    Rng *r = &G->rs;
    float W = v->site.w, D = v->site.d, ceil = ceiling_of(v);
    float L = rng_range(r, V_BED_L_LO, V_BED_L_HI), Bw = rng_range(r, V_BED_W_LO, V_BED_W_HI), bh = rng_range(r, V_BED_H_LO, V_BED_H_HI);
    float hl = rng_range(r, V_HANDLE_LO, minf(V_HANDLE_HI, V_BARROW_LEN - 0.05f - L));
    if (L + hl > W - 0.1f) { L = maxf(V_BED_L_LO, W - 0.15f - hl); if (L + hl > W - 0.1f) { hl = maxf(V_HANDLE_LO, W - 0.15f - L); if (L + hl > W - 0.1f) return 0; } }
    if (D < Bw + 0.6f || ceil < 2.5f) return 0;
    float bx0 = -0.5f * (L + hl) + hl, bx1 = bx0 + L;
    /* the variant first: a parasol needs the bed set where its canopy fits the site */
    float w4[4] = { 0.3f, 0.3f, 0.2f, 0.2f };
    if (V_TRADES[v->trade].cooking == 2) { w4[0] = 0.75f; w4[1] = 0.12f; w4[2] = 0.03f; w4[3] = 0.1f; }
    int var = v_pick(r, w4, 4);
    float pR = minf(minf(V_PARASOL_R, rng_range(r, 1.1f, V_PARASOL_R)), minf(0.5f * W - 0.05f - 0.5f * hl, 0.5f * D - 0.05f));
    if (var == 1 && pR < 0.9f) var = 3;
    float zb = minf(rng_range(r, 0.18f, 0.6f), D - Bw - 0.55f);
    if (var == 1) zb = minf(maxf(zb, pR + 0.02f - 0.5f * Bw), D - 0.05f - pR - 0.5f * Bw);
    if (zb < 0.15f) zb = 0.15f;
    if (D - (zb + Bw) < 0.5f) return 0;
    float zf = zb + Bw;
    v->front = zf;
    /* the bed and its rim */
    int bed = v_box(v, VR_BED, VS_TIMBER, VCOL_BODY, bx0, bh - 0.1f, zb, bx1, bh, zf);
    if (rng_float(r) < 0.6f) {
        v_box(v, VR_SHELL, VS_FINISH, VCOL_BODY, bx0, bh, zb, bx1, bh + 0.08f, zb + 0.04f);
        v_box(v, VR_SHELL, VS_FINISH, VCOL_BODY, bx0, bh, zf - 0.04f, bx1, bh + 0.08f, zf);
    }
    /* two wheels nearer the far end, their tops no higher than the rule allows */
    float wr = rng_range(r, V_WHEEL_LO, minf(V_WHEEL_HI, 0.5f * (bh + V_WHEEL_OVER) - 0.005f));
    float wx = bx0 + L * rng_range(r, 0.55f, 0.72f);
    for (int s = 0; s < 2; ++s) {
        float z = s ? zf + 0.05f : zb - 0.05f;
        int wh = v_cyl(v, VR_WHEEL, VS_RUBBER, VCOL_TRIM, wx, wr, z - 0.03f, wx, wr, z + 0.03f, wr, 10);
        if (wh >= 0 && bed >= 0) v_edge(v, wh, bed, VE_SUPPORTS);
    }
    v_beam(v, VR_SHELL, VS_STEEL, VCOL_TRIM, wx, wr, zb - 0.05f, wx, wr, zf + 0.05f, 0.03f);
    for (int s = 0; s < 2; ++s) {
        float z = s ? zf - 0.08f : zb + 0.08f;
        int leg = v_beam(v, VR_SUPPORT, VS_STEEL, VCOL_TRIM, bx0 + 0.12f, bh - 0.1f, z, bx0 + 0.12f, 0.0f, z, 0.035f);
        if (leg >= 0 && bed >= 0) v_edge(v, leg, bed, VE_SUPPORTS);
        int h = v_beam(v, VR_HANDLE, VS_TIMBER, VCOL_TRIM, bx0, bh - 0.05f, z, bx0 - hl, bh + rng_range(r, 0.05f, 0.15f), z, 0.035f);
        if (h >= 0 && bed >= 0) v_edge(v, h, bed, VE_ATTACHED_TO);
    }
    v->variant = var;
    v->layout = var;
    uint32_t lc = lamp_rgb(v);
    float bedTopZ0 = zb + 0.05f;
    if (var == 0 || ((var == 1 || var == 3) && rng_float(r) < 0.4f)) {
        /* a back cabinet with painted shelves */
        float cdp = var == 0 ? Bw * rng_range(r, 0.38f, 0.48f) : Bw * 0.35f, chh = var == 0 ? rng_range(r, 0.6f, 0.85f) : 0.45f;
        int cab = v_box(v, VR_CABINET, VS_FINISH, VCOL_BODY, bx0 + 0.05f, bh, zb + 0.045f, bx1 - 0.05f, bh + chh, zb + cdp);
        if (cab >= 0) {
            v->part[cab].faces = VB_BOT;
            v_opening(v, VO_CABINET, bx0 + 0.15f, bh + 0.06f, zb + cdp + 0.003f, 1.0f, 0.0f, L - 0.3f, chh - 0.12f, cdp - 0.05f, cab);
            if (bed >= 0) v_edge(v, cab, bed, VE_MOUNTED_ON);
        }
        bedTopZ0 = zb + cdp + 0.03f;
        add_surface(F, VSU_SHELF, bx0 + 0.1f, zb + 0.05f, bx1 - 0.1f, zb + cdp - 0.05f, bh + chh);
        if (var == 0) {
            /* a scalloped awning over the bed on two uprights, its lip at least V_HOOD_LIP up */
            float yTop = minf(bh + chh + rng_range(r, 0.75f, 1.0f), ceil - 0.1f), lip = maxf(rng_range(r, 2.0f, 2.2f), V_HOOD_LIP + 0.17f);
            if (yTop - lip < V_FALL_MIN) yTop = lip + V_FALL_MIN + 0.02f;
            if (yTop > ceil - 0.05f) return 0;
            float az = zf + rng_range(r, 0.15f, 0.35f);
            float q[4][3] = { { bx0 - 0.05f, yTop, zb }, { bx1 + 0.05f, yTop, zb }, { bx1 + 0.05f, lip, az }, { bx0 - 0.05f, lip, az } };
            int aw = v_quad(v, VR_AWNING, VS_STRIPE, VCOL_ACC1, q, VPF_TWOSIDED);
            float vq[4][3] = { { bx0 - 0.05f, lip, az }, { bx1 + 0.05f, lip, az }, { bx1 + 0.05f, lip - 0.15f, az }, { bx0 - 0.05f, lip - 0.15f, az } };
            int val = v_quad(v, VR_CLOTH, VS_STRIPE, VCOL_ACC1, vq, VPF_TWOSIDED | VPF_SCALLOP | VPF_SWAY);
            if (val >= 0 && aw >= 0) v_edge(v, val, aw, VE_ATTACHED_TO);
            if (cab >= 0 && aw >= 0) v_edge(v, cab, aw, VE_SUPPORTS);
            for (int s = 0; s < 2; ++s) {
                int u = v_beam(v, VR_SUPPORT, VS_STEEL, VCOL_TRIM, s ? bx1 - 0.04f : bx0 + 0.04f, bh, zb + 0.03f, s ? bx1 - 0.04f : bx0 + 0.04f, yTop, zb + 0.03f, 0.03f);
                if (u >= 0 && aw >= 0) v_edge(v, u, aw, VE_SUPPORTS);
            }
            v->clearMin = V_HOOD_LIP;
            v->roofY = lip - 0.15f;
            F->roofTop = yTop; F->roofPart = aw;
            v_sign(v, VSG_HOOD, 0.0f, 0.5f * (yTop + lip) + 0.02f, 0.5f * (zb + az) + 0.04f, 0.0f, 1.0f, minf(L * 0.7f, 1.4f), 0.3f, 0);
            int b = v_box(v, VR_LAMP, VS_BULB, VCOL_RGB, -0.05f, lip + 0.1f, zf - 0.1f, 0.05f, lip + 0.2f, zf);
            if (b >= 0) { v->part[b].rgb = lc; v->part[b].flags |= VPF_EMISSIVE; }
            v_light(v, v->cls == VC_POOR ? VLK_BULB : VLK_LED, 0.0f, lip + 0.1f, zf - 0.05f, 0.5f, lc);
            add_rail(F, bx0 + 0.1f, az - 0.12f, bx1 - 0.1f, az - 0.12f, lip - 0.02f, 1);
            if (V_TRADES[v->trade].cooking == 2) {
                float fx = bx1 - 0.25f, fz = zb + 0.15f;
                int fl = v_cyl(v, VR_FLUE, VS_STEEL, VCOL_TRIM, fx, bh + chh, fz, fx, minf(yTop + rng_range(r, 0.65f, 0.85f), ceil - 0.05f), fz, 0.05f, 6);
                if (fl >= 0 && cab >= 0) v_edge(v, fl, cab, VE_MOUNTED_ON);
            }
        }
    }
    if (var == 1) {
        /* a parasol from the bed's middle, as wide as the site allows */
        float pz = 0.5f * (zb + zf);
        float R = minf(pR, minf(D - pz - 0.05f, pz - 0.02f));
        float rim = rng_range(r, 2.05f, 2.25f), apex = rim + 0.35f;
        if (R < 0.9f || apex > ceil - 0.05f) return 0;
        float px = 0.5f * (bx0 + bx1);
        int pole = v_cyl(v, VR_SUPPORT, VS_CHROME, VCOL_TRIM, px, bh, pz, px, apex, pz, 0.02f, 6);
        int ps = v_part(v, VP_CONE, VR_ROOF, VS_STRIPE, VCOL_ACC1, VL_SHELL);
        if (ps >= 0) {
            VPart *p = &v->part[ps];
            p->a[0] = px; p->a[1] = apex; p->a[2] = pz; p->b[0] = px; p->b[1] = rim; p->b[2] = pz; p->r = R; p->segs = 10; p->flags |= VPF_TWOSIDED;
        }
        if (pole >= 0 && ps >= 0) v_edge(v, pole, ps, VE_SUPPORTS);
        v->clearMin = V_PARASOL_RIM;
        v->roofY = rim;
        F->roofTop = apex; F->roofPart = ps;
        v_sign(v, VSG_POLE, px + 0.18f, 1.75f, pz, 0.0f, 1.0f, 0.3f, 0.4f, 0);
        v_light(v, VLK_BULB, px, rim - 0.1f, pz, 0.4f, lc);
        int b = v_box(v, VR_LAMP, VS_BULB, VCOL_RGB, px - 0.05f, rim - 0.18f, pz - 0.05f, px + 0.05f, rim - 0.08f, pz + 0.05f);
        if (b >= 0) { v->part[b].rgb = lc; v->part[b].flags |= VPF_EMISSIVE; }
    } else if (var == 2) {
        /* a hanging rail on two uprights */
        float ry = rng_range(r, V_RAIL_LO + 0.01f, V_RAIL_HI - 0.01f), rz = 0.5f * (zb + zf);
        int u0 = v_beam(v, VR_SUPPORT, VS_STEEL, VCOL_TRIM, bx0 + 0.05f, bh, rz, bx0 + 0.05f, ry, rz, 0.03f);
        int u1 = v_beam(v, VR_SUPPORT, VS_STEEL, VCOL_TRIM, bx1 - 0.05f, bh, rz, bx1 - 0.05f, ry, rz, 0.03f);
        int rail = v_beam(v, VR_RAIL, VS_STEEL, VCOL_TRIM, bx0 + 0.02f, ry, rz, bx1 - 0.02f, ry, rz, 0.025f);
        if (rail >= 0) { if (u0 >= 0) v_edge(v, u0, rail, VE_SUPPORTS); if (u1 >= 0) v_edge(v, u1, rail, VE_SUPPORTS); }
        add_rail(F, bx0 + 0.15f, rz, bx1 - 0.15f, rz, ry - 0.03f, 0);
        v_sign(v, VSG_POLE, bx1 - 0.05f, ry - 0.35f, rz + 0.05f, 0.0f, 1.0f, 0.35f, 0.45f, 0);
        v_light(v, VLK_LANTERN, bx1 - 0.05f, ry - 0.1f, rz + 0.1f, 0.35f, 0xFFB060u);
        v->clearMin = V_CLEAR_MIN;
        F->roofTop = ry; F->roofPart = rail;
    } else if (var == 3) {
        /* open: a sign on a pole at one corner, a lantern on it */
        float px = bx1 - 0.06f, pz = zb + 0.06f;
        int pole = v_beam(v, VR_SUPPORT, VS_STEEL, VCOL_TRIM, px, bh, pz, px, bh + 1.5f, pz, 0.03f);
        (void)pole;
        v_sign(v, VSG_POLE, px, bh + 1.25f, pz + 0.03f, 0.0f, 1.0f, 0.5f, 0.4f, 0);
        v_light(v, VLK_LANTERN, px, bh + 1.5f, pz, 0.35f, 0xFFB060u);
        v->clearMin = V_CLEAR_MIN;
        F->roofTop = bh + 1.5f; F->roofPart = -1;
    }
    add_surface(F, VSU_BED, bx0 + 0.08f, bedTopZ0, bx1 - 0.08f, zf - 0.06f, bh);
    if (V_TRADES[v->trade].cooking && F->cook[1] < 0.0f) { F->cook[0] = 0.5f * (bx0 + bx1); F->cook[1] = bh; F->cook[2] = 0.5f * (bedTopZ0 + zf); }
    F->front = zf;
    F->signY = 0.0f;
    v_slot(v, VSL_GROUND, bx0 + 0.45f * L, 0.0f, 0.5f * (zb + zf), 0.0f, 1.0f, 0.4f, bh - 0.15f);
    v->box[0] = bx0 - hl; v->box[1] = zb - 0.12f; v->box[2] = bx1; v->box[3] = zf + 0.12f;
    for (int k = 0; k < v->npart; ++k) {
        const VPart *p = &v->part[k];
        if (p->shape == VP_CONE) { v->box[0] = minf(v->box[0], p->b[0] - p->r); v->box[2] = maxf(v->box[2], p->b[0] + p->r); v->box[1] = minf(v->box[1], p->b[2] - p->r); v->box[3] = maxf(v->box[3], p->b[2] + p->r); }
        if (p->role == VR_AWNING) v->box[3] = maxf(v->box[3], p->q[2][2]);
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
    case VA_LEANTO:    return build_leanto(G, F);
    case VA_CONTAINER: return build_container(G, F);
    case VA_PAIR:      return build_pair(G, F);
    case VA_MARKET:    return build_market(G, F);
    case VA_BARROW:    return build_barrow(G, F);
    default:           return 0;
    }
}

void vendor_min_site(int a, float *w, float *d) {
    static const float M[VA_COUNT][2] = {
        { 2.2f, 2.4f }, { 1.9f, 1.65f }, { 2.5f, 2.4f }, { V_BOX20 + 0.3f, V_BOX_W + 1.3f },
        { V_BOX20 + 0.3f, V_BOX_W + V_BOX10 + 0.3f }, { 2.2f, 2.4f }, { 2.0f, 1.7f },   /* a barrow: its bed up to 1 m wide, 0.65 m round it */
        { 2.2f, 2.2f }, { 4.0f, 3.4f } };   /* SF2: the canopy bar and hatch kiosk on the kit, the arcade counter, the tiered kiosk */
    *w = a >= 0 && a < VA_COUNT ? M[a][0] : 2.0f;
    *d = a >= 0 && a < VA_COUNT ? M[a][1] : 2.0f;
}
