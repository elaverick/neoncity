#include "vendor_internal.h"
#include <string.h>

/* ===========================================================================
 * Validation: the bounded rules (section 4 of docs/street-vendor-generation.md),
 * checked from the description alone, whatever built it. Each failure is
 * logged with the rule and the measure, as the doc asks ("footprint is 6.7 m
 * deep, leaving under 1.5 m of footway"), in the tower grammar's vocabulary.
 * ========================================================================= */

#define EPS 0.005f
#define CONTACT 0.08f

typedef struct VCheck { const Vendor *v; BLog *log; int errors; } VCheck;

static void fail(VCheck *K, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    blog_printf(K->log, "    reject: ");
    blog_vprintf(K->log, fmt, ap);
    blog_printf(K->log, "\n");
    va_end(ap);
    K->errors++;
}

static float plen(const float a[3], const float b[3]) {
    float dx = b[0] - a[0], dy = b[1] - a[1], dz = b[2] - a[2];
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

/* The part's box on the ground (x0, z0, x1, z1). */
static void foot_of(const VPart *p, float f[4]) {
    float mn[3], mx[3];
    v_part_aabb(p, mn, mx);
    f[0] = mn[0]; f[1] = mn[2]; f[2] = mx[0]; f[3] = mx[2];
}

/* Spans and cantilevers of a roof along one axis (0 x, 2 z): the supports' intervals
 * that reach under it, sorted; the widest gap between them and the overhang at each end. */
static void spans(const Vendor *v, int roof, int axis, float *gap, float *over) {
    float rf[4], lo[32], hi[32];
    int n = 0;
    foot_of(&v->part[roof], rf);
    float r0 = axis == 0 ? rf[0] : rf[1], r1 = axis == 0 ? rf[2] : rf[3];
    float o0 = axis == 0 ? rf[1] : rf[0], o1 = axis == 0 ? rf[3] : rf[2];
    for (int e = 0; e < v->nedge && n < 32; ++e) {
        if (v->edge[e].kind != VE_SUPPORTS || v->edge[e].to != roof) continue;
        float sf[4];
        foot_of(&v->part[v->edge[e].from], sf);
        float s0 = axis == 0 ? sf[0] : sf[1], s1 = axis == 0 ? sf[2] : sf[3];
        float t0 = axis == 0 ? sf[1] : sf[0], t1 = axis == 0 ? sf[3] : sf[2];
        /* a support carries the roof where it reaches under it or meets its edge (within CONTACT) */
        if (s1 < r0 - CONTACT || s0 > r1 + CONTACT || t1 < o0 - CONTACT || t0 > o1 + CONTACT) continue;
        lo[n] = minf(maxf(s0, r0), r1); hi[n] = maxf(minf(s1, r1), lo[n]); n++;
    }
    *gap = 0.0f; *over = 0.0f;
    if (!n) { *over = r1 - r0; return; }
    for (int i = 1; i < n; ++i)
        for (int k = i; k > 0 && lo[k] < lo[k - 1]; --k) { float t = lo[k]; lo[k] = lo[k - 1]; lo[k - 1] = t; t = hi[k]; hi[k] = hi[k - 1]; hi[k - 1] = t; }
    float reach = hi[0];
    for (int i = 1; i < n; ++i) {
        if (lo[i] > reach) *gap = maxf(*gap, lo[i] - reach);
        reach = maxf(reach, hi[i]);
    }
    *over = maxf(lo[0] - r0, r1 - reach);
}

/* The underside of a roof-like part over (x, z), if it is over it. */
static int under_at(const VPart *p, float x, float z, float *y) {
    switch (p->shape) {
    case VP_BOX:
        if (x < p->a[0] || x > p->b[0] || z < p->a[2] || z > p->b[2]) return 0;
        *y = p->a[1];
        return 1;
    case VP_VAULT: {
        if (x < p->a[0] || x > p->b[0] || z < p->a[2] || z > p->b[2]) return 0;
        float t = (z - p->a[2]) / maxf(p->b[2] - p->a[2], 1e-4f);
        *y = p->a[1] + p->r * sinf(PI_F * t);
        return 1;
    }
    case VP_CONE: {
        float dx = x - p->b[0], dz = z - p->b[2], rr = sqrtf(dx * dx + dz * dz);
        if (rr > p->r) return 0;
        *y = p->b[1] + (p->a[1] - p->b[1]) * (1.0f - rr / p->r);
        return 1;
    }
    case VP_QUAD: {
        /* two triangles q0 q1 q2 and q0 q2 q3, seen from above */
        for (int t = 0; t < 2; ++t) {
            const float *A = p->q[0], *B = p->q[t ? 2 : 1], *C = p->q[t ? 3 : 2];
            float d = (B[2] - C[2]) * (A[0] - C[0]) + (C[0] - B[0]) * (A[2] - C[2]);
            if (fabsf(d) < 1e-8f) continue;
            float l0 = ((B[2] - C[2]) * (x - C[0]) + (C[0] - B[0]) * (z - C[2])) / d;
            float l1 = ((C[2] - A[2]) * (x - C[0]) + (A[0] - C[0]) * (z - C[2])) / d;
            float l2 = 1.0f - l0 - l1;
            if (l0 < -1e-4f || l1 < -1e-4f || l2 < -1e-4f) continue;
            *y = l0 * A[1] + l1 * B[1] + l2 * C[1];
            return 1;
        }
        return 0;
    }
    default:
        return 0;
    }
}

static int is_cover(int role) { return role == VR_ROOF || role == VR_AWNING || role == VR_FLAP || role == VR_CLOTH || role == VR_HOOD; }

int vendor_validate(const Vendor *v, const VendorFinger *ctx, int n, BLog *log) {
    VCheck K = { v, log, 0 };
    const VendorSite *s = &v->site;
    float ceil = s->ceiling > 0.0f ? s->ceiling : 1e9f;
    const float hw = 0.5f * s->w;

    /* the site: everything inside its rectangle and under its ceiling */
    if (v->box[0] < -hw - EPS || v->box[2] > hw + EPS) fail(&K, "the vendor is %.2f m wide, the site %.2f m", v->box[2] - v->box[0], s->w);
    if (v->box[1] < -EPS || v->box[3] > s->d + EPS) fail(&K, "footprint reaches %.2f to %.2f m deep on a %.2f m site, leaving no footway", v->box[1], v->box[3], s->d);
    if (v->top > ceil + EPS) fail(&K, "it reaches %.2f m under a %.2f m ceiling", v->top, ceil);

    for (int i = 0; i < v->npart; ++i) {
        const VPart *p = &v->part[i];
        int nsup = 0, attached = 0;
        for (int e = 0; e < v->nedge; ++e) {
            if (v->edge[e].to == i && v->edge[e].kind == VE_SUPPORTS) nsup++;
            if (v->edge[e].from == i && (v->edge[e].kind == VE_ATTACHED_TO || v->edge[e].kind == VE_MOUNTED_ON)) attached++;
        }
        switch (p->role) {
        case VR_ROOF: case VR_AWNING:
            if (p->shape == VP_CONE) {
                if (p->r > V_PARASOL_R + EPS) fail(&K, "parasol radius %.2f m over %.1f m", p->r, V_PARASOL_R);
                if (p->b[1] < V_PARASOL_RIM - EPS) fail(&K, "parasol rim %.2f m up, under %.1f m", p->b[1], V_PARASOL_RIM);
                if (!nsup) fail(&K, "a parasol without a pole");
                break;
            }
            if (!nsup) { if (!attached) fail(&K, "part %d (%s) stands on nothing", i, p->role == VR_ROOF ? "roof" : "awning"); break; }
            for (int ax = 0; ax <= 2; ax += 2) {
                float gap, over;
                spans(v, i, ax, &gap, &over);
                if (gap > V_SPAN_MAX + EPS) fail(&K, "unbraced span %.2f m over %.1f m (%s)", gap, V_SPAN_MAX, ax ? "deep" : "across");
                if (over > V_CANTILEVER_MAX + EPS) fail(&K, "cantilever %.2f m beyond its last support, over %.1f m (%s)", over, V_CANTILEVER_MAX, ax ? "deep" : "across");
            }
            if (p->shape == VP_QUAD) {
                float fall = p->b[1] - p->a[1];
                if (fall < V_FALL_MIN - EPS && (v->archetype == VA_MARKET || v->archetype == VA_BARROW || v->archetype == VA_LEANTO || v->archetype == VA_PAIR))
                    fail(&K, "canopy falls %.2f m, under %.2f m: the rain won't shed", fall, V_FALL_MIN);
            }
            if (p->role == VR_AWNING && p->shape == VP_QUAD) {
                float dz = fabsf(0.5f * (p->q[2][2] + p->q[3][2]) - 0.5f * (p->q[0][2] + p->q[1][2])) + fabsf(0.5f * (p->q[2][0] + p->q[3][0]) - 0.5f * (p->q[0][0] + p->q[1][0]));
                if (dz > V_AWNING_MAX + EPS) fail(&K, "cloth awning %.2f m deep, over %.1f m", dz, V_AWNING_MAX);
            }
            break;
        case VR_FLAP: {
            float L = 0.5f * (plen(p->q[0], p->q[3]) + plen(p->q[1], p->q[2]));
            if (L > V_FLAP_MAX + EPS) fail(&K, "hinged flap %.2f m deep, over %.1f m", L, V_FLAP_MAX);
            if (nsup < 1) fail(&K, "a flap without struts");
            break;
        }
        case VR_COUNTER: {
            float h = p->b[1], d = minf(p->b[0] - p->a[0], p->b[2] - p->a[2]);
            if (h < V_COUNTER_LO - EPS || h > V_COUNTER_HI + EPS) fail(&K, "counter %.2f m high, outside %.2f-%.2f m", h, V_COUNTER_LO, V_COUNTER_HI);
            if (d < V_COUNTER_D_LO - EPS || d > V_COUNTER_D_HI + EPS) fail(&K, "counter %.2f m deep, outside %.2f-%.2f m", d, V_COUNTER_D_LO, V_COUNTER_D_HI);
            break;
        }
        case VR_TABLE: {
            float h = p->b[1], d = p->b[2] - p->a[2];
            if (h < V_TABLE_LO - EPS || h > V_TABLE_HI + EPS) fail(&K, "market table %.2f m high, outside %.1f-%.1f m", h, V_TABLE_LO, V_TABLE_HI);
            if (d < V_TABLE_D_LO - EPS || d > V_TABLE_D_HI + EPS) fail(&K, "market table %.2f m deep, outside %.1f-%.1f m", d, V_TABLE_D_LO, V_TABLE_D_HI);
            break;
        }
        case VR_FLUE: {
            /* above every roof it passes through by V_FLUE_ABOVE */
            float top = maxf(p->a[1], p->b[1]), x = p->b[0], z = p->b[2], roof = -1.0f;
            for (int k = 0; k < v->npart; ++k) {
                const VPart *r = &v->part[k];
                if (r->role != VR_ROOF && r->role != VR_AWNING) continue;
                float rf[4];
                foot_of(r, rf);
                if (x < rf[0] || x > rf[2] || z < rf[1] || z > rf[3]) continue;
                roof = maxf(roof, maxf(r->a[1], r->b[1]));
            }
            for (int k = 0; k < v->ncbox; ++k)
                if (x > v->cbox[k][0] && x < v->cbox[k][2] && z > v->cbox[k][1] && z < v->cbox[k][3]) roof = maxf(roof, v->cbox[k][5]);
            if (roof > 0.0f && top < roof + V_FLUE_ABOVE - EPS && top < ceil - 0.1f) fail(&K, "flue %.2f m above the roof, under %.1f m", top - roof, V_FLUE_ABOVE);
            break;
        }
        case VR_BED: {
            float L = p->b[0] - p->a[0], W = p->b[2] - p->a[2], H = p->b[1];
            if (L < V_BED_L_LO - EPS || L > V_BED_L_HI + EPS) fail(&K, "barrow bed %.2f m long, outside %.1f-%.1f m", L, V_BED_L_LO, V_BED_L_HI);
            if (W < V_BED_W_LO - EPS || W > V_BED_W_HI + EPS) fail(&K, "barrow bed %.2f m wide, outside %.1f-%.1f m", W, V_BED_W_LO, V_BED_W_HI);
            if (H < V_BED_H_LO - EPS || H > V_BED_H_HI + EPS) fail(&K, "barrow bed %.2f m high, outside %.2f-%.2f m", H, V_BED_H_LO, V_BED_H_HI);
            for (int k = 0; k < v->npart; ++k) {
                const VPart *w = &v->part[k];
                if (w->role == VR_WHEEL) {
                    if (w->r < V_WHEEL_LO - EPS || w->r > V_WHEEL_HI + EPS) fail(&K, "wheel radius %.2f m, outside %.2f-%.2f m", w->r, V_WHEEL_LO, V_WHEEL_HI);
                    if (w->a[1] + w->r > H + V_WHEEL_OVER + EPS) fail(&K, "wheel top %.2f m over the bed, over %.2f m", w->a[1] + w->r - H, V_WHEEL_OVER);
                }
                if (w->role == VR_HANDLE) {
                    float hl = plen(w->a, w->b);
                    if (hl < V_HANDLE_LO - EPS || hl > V_HANDLE_HI + 0.05f) fail(&K, "handle %.2f m, outside %.1f-%.1f m", hl, V_HANDLE_LO, V_HANDLE_HI);
                }
                if (w->role == VR_RAIL && (w->a[1] < V_RAIL_LO - EPS || w->a[1] > V_RAIL_HI + EPS)) fail(&K, "rail at %.2f m, outside %.1f-%.1f m", w->a[1], V_RAIL_LO, V_RAIL_HI);
            }
            float x0 = 1e9f, x1 = -1e9f;
            for (int k = 0; k < v->npart; ++k) {
                const VPart *w = &v->part[k];
                if (w->role != VR_BED && w->role != VR_HANDLE && w->role != VR_WHEEL) continue;
                float f[4];
                foot_of(w, f);
                x0 = minf(x0, f[0]); x1 = maxf(x1, f[2]);
            }
            if (x1 - x0 > V_BARROW_LEN + 0.05f) fail(&K, "barrow %.2f m long with its handles, over %.1f m", x1 - x0, V_BARROW_LEN);
            break;
        }
        default:
            break;
        }
    }

    /* clear height where customers stand, under every roof, awning, flap and valance */
    if (v->front < s->d - 0.3f) {
        float need = v->clearMin > 0.0f ? v->clearMin : V_CLEAR_MIN;
        float worst = 1e9f, wx = 0.0f, wz = 0.0f;
        for (float z = v->front + 0.35f; z <= s->d + 1e-3f; z += 0.25f)
            for (float x = -hw + 0.1f; x <= hw - 0.1f + 1e-3f; x += 0.25f)
                for (int i = 0; i < v->npart; ++i) {
                    const VPart *p = &v->part[i];
                    float y;
                    if (!is_cover(p->role) || !under_at(p, x, z, &y)) continue;
                    if (p->role == VR_CLOTH && !(p->flags & VPF_SCALLOP)) continue;   /* side cloths hang at the sides */
                    if (p->role == VR_CLOTH) y = minf(minf(p->q[2][1], p->q[3][1]), y);
                    if (y < worst) { worst = y; wx = x; wz = z; }
                }
        if (worst < need - EPS) fail(&K, "clear height %.2f m at (%.1f, %.1f) where people stand, under %.2f m", worst, wx, wz, need);
    }

    /* items: hangings clear of heads, stools at their pitch, gas bottles, emitters */
    int gas = 0;
    for (int i = 0; i < v->nitem; ++i) {
        const VItem *it = &v->item[i];
        if (it->role == VR_HANG && it->kind != VI_BUNTING && it->p[2] > v->front + 0.05f && it->p[1] - it->s[1] < V_HANG_CLEAR - EPS)
            fail(&K, "%s hangs to %.2f m over the footway, under %.2f m", vendor_item_name(it->kind), it->p[1] - it->s[1], V_HANG_CLEAR);
        if (it->kind == VI_GASBOTTLE || it->kind == VI_DGAS) gas++;
        if (it->role != VR_STOOL) continue;
        for (int k = i + 1; k < v->nitem; ++k) {
            const VItem *o = &v->item[k];
            if (o->role != VR_STOOL) continue;
            float dx = o->p[0] - it->p[0], dz = o->p[2] - it->p[2];
            if (dx * dx + dz * dz < (V_STOOL_PITCH - EPS) * (V_STOOL_PITCH - EPS)) fail(&K, "stools %.2f m apart, under %.2f m", sqrtf(dx * dx + dz * dz), V_STOOL_PITCH);
        }
    }
    if (gas > V_GAS_MAX) fail(&K, "%d gas bottles, over %d", gas, V_GAS_MAX);
    if (v->nemit > V_EMITTERS_MAX) fail(&K, "%d steam emitters, over %d", v->nemit, V_EMITTERS_MAX);

    /* signs */
    for (int i = 0; i < v->nsign; ++i) {
        const VSign *g = &v->sign[i];
        if (g->kind == VSG_BLADE) {
            if (g->w > V_BLADE_OUT + EPS) fail(&K, "blade sign projects %.2f m, over %.1f m", g->w, V_BLADE_OUT);
            if (g->c[1] - 0.5f * g->h < V_BLADE_CLEAR - EPS) fail(&K, "blade sign %.2f m up, under %.1f m", g->c[1] - 0.5f * g->h, V_BLADE_CLEAR);
        }
        if (g->kind == VSG_ROOF && g->w * g->h > V_ROOFSIGN_AREA + EPS) fail(&K, "roof sign %.2f m2, over %.1f m2", g->w * g->h, V_ROOFSIGN_AREA);
    }

    /* containers: openings, corner posts, sills, head rails; the court */
    for (int i = 0; i < v->nopen; ++i) {
        const VOpening *o = &v->open[i];
        if (o->cbox == 255) continue;
        const float *c = v->cbox[o->cbox];
        if (o->kind == VO_HATCH && o->o[1] - (c[4] + 0.15f) < V_SILL_MIN - EPS) fail(&K, "container sill %.2f m, under %.1f m", o->o[1] - (c[4] + 0.15f), V_SILL_MIN);
        if (c[5] - (o->o[1] + o->h) < V_HEAD_RAIL - EPS) fail(&K, "container head rail %.2f m, under %.1f m", c[5] - (o->o[1] + o->h), V_HEAD_RAIL);
    }
    for (int b = 0; b < v->ncbox; ++b) {
        const float *c = v->cbox[b];
        int alongX = v->cboxAxis[b] == 0;
        float L = alongX ? c[2] - c[0] : c[3] - c[1], base = alongX ? c[0] : c[1], total = 0.0f, first = 1e9f, last = -1e9f;
        for (int i = 0; i < v->nopen; ++i) {
            const VOpening *o = &v->open[i];
            if (o->cbox != b) continue;
            float a = (alongX ? o->o[0] : o->o[2]) - base;
            total += o->w; first = minf(first, a); last = maxf(last, a + o->w);
        }
        if (total <= 0.0f) continue;
        if (total > V_OPENING_SHARE * L + EPS) fail(&K, "container openings %.2f m of %.2f m, over %.0f%%", total, L, V_OPENING_SHARE * 100.0f);
        if (first < V_CORNER_POST - EPS || L - last < V_CORNER_POST - EPS) fail(&K, "container corner post %.2f m, under %.1f m", minf(first, L - last), V_CORNER_POST);
    }
    if (v->archetype == VA_PAIR && v->court > V_COURT_MAX + EPS) fail(&K, "covered court %.2f m across, over %.1f m", v->court, V_COURT_MAX);

    /* the budget */
    if (v->st.tris > V_TIER[v->tier].tris) fail(&K, "%d triangles, over the %s tier's %d", v->st.tris, vendor_tier_name(v->tier), V_TIER[v->tier].tris);
    if (v->st.hangings > V_TIER[v->tier].hangings) fail(&K, "%d hanging items, over %d", v->st.hangings, V_TIER[v->tier].hangings);
    if (v->st.props > V_TIER[v->tier].props) fail(&K, "%d props, over %d", v->st.props, V_TIER[v->tier].props);
    if (v->st.decor > V_TIER[v->tier].decor) fail(&K, "%d decorations, over %d", v->st.decor, V_TIER[v->tier].decor);
    if (v->st.neon > V_TIER[v->tier].neon) fail(&K, "%d neon, over %d", v->st.neon, V_TIER[v->tier].neon);

    /* uniqueness (section 8) */
    for (int i = 0; i < n; ++i) {
        int sh = vendor_finger_shared(&v->finger, &ctx[i]);
        if (sh > V_FINGER_SHARE) fail(&K, "shares %d of %d axes with neighbour %d, over %d", sh, V_FINGER_AXES, i, V_FINGER_SHARE);
        if (ctx[i].signature == v->finger.signature && v->finger.signature < VSIG_COUNT) fail(&K, "the same signature (%s) as neighbour %d", vendor_signature_name(v->finger.signature), i);
    }
    return K.errors;
}
