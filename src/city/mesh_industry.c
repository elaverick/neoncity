#include "city_internal.h"

/* ===========================================================================
 * Industrial plants and substations (src/gen/building_industry.c) as
 * geometry, in the building's frame: vessels, stacks, flares, gasholders,
 * frames, racks and decks, transformers and gantries, pipes and conductors,
 * ladders and stairs, the fence and the lamps. Stacks' plumes and flares'
 * fire are volumes of their own (push_plume, push_flame). The buildings
 * among them (control buildings, halls, warehouses, offices, switch houses) are facades,
 * emitted with every other building's (emit_building).
 *
 * MAT_INDUSTRY vertices (shaders/city/industry.glsl): params y the surface
 * (IND_*), z the paint (PA_*) | pattern; uv
 * metres round or along a surface, and height.
 * ========================================================================= */

enum { IP_HOOPS = 16, IP_RIBS = 32, IP_GRIME = 64 };   /* MAT_INDUSTRY params z: pattern bits over the paint */

static Mat imat(int surf, int paint, int pattern, uint32_t seed) {
    return mat(MAT_INDUSTRY, seed, (uint8_t)surf, (uint8_t)(paint | pattern), 0);
}

static int steel_of(const BGraph *g) { return g->style == PST_GAS ? PA_GALV : g->style == PST_GOTHIC ? PA_SOOT : PA_STEEL; }
static int pipe_of(const BGraph *g) { return g->style == PST_GAS ? PA_YELLOW : g->style == PST_GOTHIC ? PA_BRASS : PA_STEEL; }
static int grime_of(const BGraph *g) { return g->style == PST_NIGHT ? IP_GRIME : 0; }
static Mat concrete(const BGraph *g) { return mat(MAT_METAL, g->seed, 5, 0, 0); }

static void box_of(const BLoop *l, float *x0, float *z0, float *x1, float *z1) {
    *x0 = *z0 = 1e30f; *x1 = *z1 = -1e30f;
    for (int i = 0; i < l->n; ++i) { *x0 = minf(*x0, l->x[i]); *x1 = maxf(*x1, l->x[i]); *z0 = minf(*z0, l->z[i]); *z1 = maxf(*z1, l->z[i]); }
}

/* A flare's timing (flameGout): its own, so neighbours don't gout together. */
static uint32_t flare_id(const BGraph *g, int si) { return hash_combine(g->seed ^ 0xF1A3u, (uint32_t)si) & 63u; }

static int facets(float r) { return r < 0.5f ? 6 : r < 2.0f ? 10 : r < 5.0f ? 16 : 24; }

/* ---------------------------------------------------------------------------
 * Curved surfaces: smooth normals per vertex
 * ------------------------------------------------------------------------- */

static void vquad(Mesh *m, vec3 a, vec3 b, vec3 c, vec3 d, vec3 na, vec3 nb, vec3 nc, vec3 nd, const float uv[4][2], Mat mt) {
    vec3 p[4] = { a, b, c, d }, n[4] = { na, nb, nc, nd };
    static const int fw[6] = { 0, 1, 2, 0, 2, 3 }, bw[6] = { 0, 2, 1, 0, 3, 2 };
    vec3 face = v3_cross(v3_sub(c, a), v3_sub(d, b));
    const int *t = v3_dot(face, v3_add(v3_add(na, nb), v3_add(nc, nd))) < 0.0f ? bw : fw;
    for (int i = 0; i < 6; ++i) mesh_push(m, p[t[i]], n[t[i]], uv[t[i]][0], uv[t[i]][1], mt.seed, mt.p, mt.aux);
}

/* The side of a frustum round (cx, cz), radius r0 at y0 to r1 at y1, facing
 * out (out > 0) or in; u runs round it in metres. */
static void frustum(Mesh *m, float cx, float cz, float r0, float r1, float y0, float y1, int n, float out, Mat mt) {
    float slope = (r0 - r1) / maxf(y1 - y0, 1e-3f), k = 1.0f / sqrtf(1.0f + slope * slope);
    float rm = 0.5f * (r0 + r1);
    for (int i = 0; i < n; ++i) {
        float a0 = 2.0f * PI_F * (float)i / (float)n, a1 = 2.0f * PI_F * (float)(i + 1) / (float)n;
        float c0 = cosf(a0), s0 = sinf(a0), c1 = cosf(a1), s1 = sinf(a1);
        vec3 n0 = v3_scale(v3(c0 * k, slope * k, s0 * k), out), n1 = v3_scale(v3(c1 * k, slope * k, s1 * k), out);
        float uv[4][2] = { { a0 * rm, y0 }, { a1 * rm, y0 }, { a1 * rm, y1 }, { a0 * rm, y1 } };
        vquad(m, v3(cx + c0 * r0, y0, cz + s0 * r0), v3(cx + c1 * r0, y0, cz + s1 * r0), v3(cx + c1 * r1, y1, cz + s1 * r1),
              v3(cx + c0 * r1, y1, cz + s0 * r1), n0, n1, n1, n0, uv, mt);
    }
}

/* A flat ring between radii r0 < r1 at height y, facing up or down (r0 = 0: a disc). */
static void ring(Mesh *m, float cx, float cz, float r0, float r1, float y, int up, int n, Mat mt) {
    vec3 nn = v3(0, up ? 1.0f : -1.0f, 0);
    for (int i = 0; i < n; ++i) {
        float a0 = 2.0f * PI_F * (float)i / (float)n, a1 = 2.0f * PI_F * (float)(i + 1) / (float)n;
        float c0 = cosf(a0), s0 = sinf(a0), c1 = cosf(a1), s1 = sinf(a1);
        vec3 p0 = v3(cx + c0 * r1, y, cz + s0 * r1), p1 = v3(cx + c1 * r1, y, cz + s1 * r1);
        if (r0 <= 0.0f) {
            float uv[3][2] = { { cx, cz }, { p0.x, p0.z }, { p1.x, p1.z } };
            emit_tri(m, v3(cx, y, cz), p0, p1, nn, uv, mt);
        } else {
            vec3 q0 = v3(cx + c0 * r0, y, cz + s0 * r0), q1 = v3(cx + c1 * r0, y, cz + s1 * r0);
            float uv[4][2] = { { q0.x, q0.z }, { p0.x, p0.z }, { p1.x, p1.z }, { q1.x, q1.z } };
            emit_quad(m, q0, p0, p1, q1, nn, uv, mt);
        }
    }
}

/* A dome (half an ellipsoid) of radius r rising h from y. */
static void dome(Mesh *m, float cx, float cz, float r, float y, float h, int n, Mat mt) {
    const int rings = 4;
    for (int k = 0; k < rings; ++k) {
        float f0 = 0.5f * PI_F * (float)k / (float)rings, f1 = 0.5f * PI_F * (float)(k + 1) / (float)rings;
        float ra = r * cosf(f0), rb = r * cosf(f1), ya = y + h * sinf(f0), yb = y + h * sinf(f1);
        for (int i = 0; i < n; ++i) {
            float a0 = 2.0f * PI_F * (float)i / (float)n, a1 = 2.0f * PI_F * (float)(i + 1) / (float)n;
            float c0 = cosf(a0), s0 = sinf(a0), c1 = cosf(a1), s1 = sinf(a1);
            vec3 n00 = v3_norm(v3(cosf(f0) * c0 / r, sinf(f0) / h, cosf(f0) * s0 / r)), n10 = v3_norm(v3(cosf(f0) * c1 / r, sinf(f0) / h, cosf(f0) * s1 / r));
            vec3 n01 = v3_norm(v3(cosf(f1) * c0 / r, sinf(f1) / h, cosf(f1) * s0 / r)), n11 = v3_norm(v3(cosf(f1) * c1 / r, sinf(f1) / h, cosf(f1) * s1 / r));
            float uv[4][2] = { { a0 * r, ya }, { a1 * r, ya }, { a1 * r, yb }, { a0 * r, yb } };
            vquad(m, v3(cx + c0 * ra, ya, cz + s0 * ra), v3(cx + c1 * ra, ya, cz + s1 * ra), v3(cx + c1 * rb, yb, cz + s1 * rb),
                  v3(cx + c0 * rb, yb, cz + s0 * rb), n00, n10, n11, n01, uv, mt);
        }
    }
}

/* A round tube of radius r from a to b, n facets, its ends open (or capped). */
static void tube(Mesh *m, vec3 a, vec3 b, float r, int n, int caps, Mat mt) {
    vec3 ax = v3_sub(b, a);
    float len = v3_len(ax);
    if (len < 1e-4f) return;
    ax = v3_scale(ax, 1.0f / len);
    vec3 h = fabsf(ax.y) < 0.9f ? v3_norm(v3_cross(ax, v3(0, 1, 0))) : v3_norm(v3_cross(ax, v3(1, 0, 0)));
    vec3 v = v3_cross(h, ax);
    for (int i = 0; i < n; ++i) {
        float a0 = 2.0f * PI_F * (float)i / (float)n, a1 = 2.0f * PI_F * (float)(i + 1) / (float)n;
        vec3 d0 = v3_add(v3_scale(h, cosf(a0)), v3_scale(v, sinf(a0))), d1 = v3_add(v3_scale(h, cosf(a1)), v3_scale(v, sinf(a1)));
        float uv[4][2] = { { a0 * r, a.y }, { a1 * r, a.y }, { a1 * r, b.y }, { a0 * r, b.y } };
        vquad(m, v3_add(a, v3_scale(d0, r)), v3_add(a, v3_scale(d1, r)), v3_add(b, v3_scale(d1, r)), v3_add(b, v3_scale(d0, r)), d0, d1, d1, d0, uv, mt);
        if (caps) {
            float u3[3][2] = { { 0, 0 }, { 1, 0 }, { 0, 1 } };
            emit_tri(m, a, v3_add(a, v3_scale(d0, r)), v3_add(a, v3_scale(d1, r)), v3_scale(ax, -1.0f), u3, mt);
            emit_tri(m, b, v3_add(b, v3_scale(d0, r)), v3_add(b, v3_scale(d1, r)), ax, u3, mt);
        }
    }
}

/* A flat quad facing both ways (two quads back to back): the fences' mesh. */
static void sheet(Mesh *m, vec3 a, vec3 b, vec3 c, vec3 d, float u1, float v1, Mat mt) {
    vec3 n = v3_norm(v3_cross(v3_sub(b, a), v3_sub(d, a)));
    float uv[4][2] = { { 0, 0 }, { u1, 0 }, { u1, v1 }, { 0, v1 } };
    emit_quad(m, a, b, c, d, n, uv, mt);
    emit_quad(m, a, b, c, d, v3_scale(n, -1.0f), uv, mt);
}

/* A railing: a wire-mesh panel and a top rail from a to b, standing h. */
static void railing(Mesh *m, const BGraph *g, vec3 a, vec3 b, float h) {
    float len = sqrtf((b.x - a.x) * (b.x - a.x) + (b.z - a.z) * (b.z - a.z));
    if (len < 0.2f) return;
    sheet(m, a, b, v3(b.x, b.y + h, b.z), v3(a.x, a.y + h, a.z), len, h, imat(IND_MESH, steel_of(g), 0, g->seed));
    emit_beam(m, v3(a.x, a.y + h, a.z), v3(b.x, b.y + h, b.z), 0.06f, imat(IND_PAINT, steel_of(g), 0, g->seed));
}

/* ---------------------------------------------------------------------------
 * Lamps (shared with the light tables, light_tiles.c)
 * ------------------------------------------------------------------------- */

static void lamp_add(ILamp *out, int *n, int max, float x, float y, float z, int kind) {
    if (*n < max) { out[*n].x = x; out[*n].y = y; out[*n].z = z; out[*n].kind = kind; (*n)++; }
}

int industry_lamps(const BGraph *g, ILamp *out, int max) {
    int n = 0;
    /* floodlight masts in the yard's corners (all four in a big one), where nothing stands */
    float fx[4] = { g->fence[0] + 0.6f, g->fence[2] - 0.6f, g->fence[2] - 0.6f, g->fence[0] + 0.6f };
    float fz[4] = { g->fence[1] + 0.6f, g->fence[3] - 0.6f, g->fence[1] + 0.6f, g->fence[3] - 0.6f };
    int masts = g->archetype == ARCH_SUBSTATION || (g->fence[2] - g->fence[0]) * (g->fence[3] - g->fence[1]) >= 1500.0f ? 4 : 2;
    for (int k = 0; k < masts; ++k) {
        int free = 1;
        for (int i = 0; i < g->nsec && free; ++i)
            free = !(g->sec[i].y0 <= G0 + 0.01f && (bfoot_contains(&g->sec[i].foot, fx[k], fz[k]) || bfoot_boundary_dist(&g->sec[i].foot, fx[k], fz[k], NULL, NULL) < 0.8f));
        if (free) lamp_add(out, &n, max, fx[k], G0 + 8.0f, fz[k], ILAMP_MAST);
    }
    int dense = g->style == PST_NIGHT ? 1 : 2;   /* lamps on every bent of a rack, or every other */
    for (int si = 0; si < g->nsec; ++si) {
        const BSection *s = &g->sec[si];
        const BLoop *l = &s->foot.outer;
        if (s->type == SEC_DECK) {
            for (int k = 0; k < l->n; k += g->style == PST_GAS ? 2 : 1) lamp_add(out, &n, max, l->x[k], s->y1 + 2.6f, l->z[k], ILAMP_SODIUM);
            if (g->style == PST_NIGHT)   /* and under it */
                for (int h = 0; h < g->nhole; ++h)
                    if (g->hole[h].deck == si) lamp_add(out, &n, max, g->hole[h].x + g->hole[h].r + 0.3f, s->y0 - 0.4f, g->hole[h].z, ILAMP_SODIUM);
            continue;
        }
        switch (s->unit) {
        case IU_COLUMN: {   /* on the platform round its top */
            float cx, cz, r;
            bloop_circle(l, &cx, &cz, &r);
            float top = s->y1 - bgen_head_height(s->head, r);
            if (top - G0 > 8.0f && !s->braceBy) lamp_add(out, &n, max, cx + r + 0.8f, top + 1.0f, cz, ILAMP_SODIUM);
            break;
        }
        case IU_FRAME: {
            int levels = (int)floorf((s->y1 - G0) / 6.0f + 0.01f);
            for (int lv = 1; lv <= levels; ++lv)
                lamp_add(out, &n, max, l->x[lv & 3], G0 + (float)lv * 6.0f + 0.5f, l->z[lv & 3], ILAMP_SODIUM);
            break;
        }
        case IU_RACK: {
            int bent = 0;
            for (int k = 0; k + 1 < g->nsup; ++k) {
                const BSupport *a = &g->sup[k];
                if (a->section != si || g->sup[k + 1].section != si) continue;
                if (bent++ % dense == 0) lamp_add(out, &n, max, a->x1, a->y1 + 0.6f, a->z1, ILAMP_SODIUM);
                ++k;
            }
            break;
        }
        case IU_FLARE: {   /* the nozzle; light_tiles.c makes it a flame's light */
            float cx, cz, r;
            bloop_circle(l, &cx, &cz, &r);
            if (n < max) { out[n].reach = flare_reach(s->y1 - G0); out[n].id = flare_id(g, si); }
            lamp_add(out, &n, max, cx, s->y1 - 0.45f, cz, ILAMP_FLARE);
            break;
        }
        case IU_GANTRY:
            lamp_add(out, &n, max, l->x[0], s->y1 + 0.5f, l->z[0], ILAMP_FLOOD);
            lamp_add(out, &n, max, l->x[2], s->y1 + 0.5f, l->z[2], ILAMP_FLOOD);
            break;
        default:
            break;
        }
    }
    return n;
}

static void emit_lamps(Mesh *m, const BGraph *g) {
    ILamp L[INDUSTRY_MAX_LAMPS];
    int n = industry_lamps(g, L, INDUSTRY_MAX_LAMPS);
    Mat post = imat(IND_PAINT, steel_of(g), 0, g->seed);
    for (int k = 0; k < n; ++k) {
        uint32_t h = hash_combine(g->seed ^ 0x1A3Bu, (uint32_t)k);
        if (L[k].kind == ILAMP_FLARE) continue;   /* the flame is its own light */
        if (L[k].kind == ILAMP_MAST) {
            emit_box(m, v3(L[k].x - 0.1f, G0, L[k].z - 0.1f), v3(L[k].x + 0.1f, L[k].y, L[k].z + 0.1f), F_SIDES, post, UV_FACADE);
            emit_box(m, v3(L[k].x - 0.5f, L[k].y, L[k].z - 0.15f), v3(L[k].x + 0.5f, L[k].y + 0.35f, L[k].z + 0.15f), F_NOBOT | F_BOT, post, UV_FACADE);
            emit_emissive_box(m, v3(L[k].x, L[k].y - 0.05f, L[k].z), 0.16f, 0, 3, (uint8_t)(h & 255u));
            continue;
        }
        emit_box(m, v3(L[k].x - 0.04f, L[k].y - 0.9f, L[k].z - 0.04f), v3(L[k].x + 0.04f, L[k].y, L[k].z + 0.04f), F_SIDES, post, UV_FACADE);
        emit_emissive_box(m, v3(L[k].x, L[k].y + 0.1f, L[k].z), 0.13f, 0, L[k].kind == ILAMP_FLOOD ? 3 : 2, (uint8_t)(h & 255u));
    }
}

/* Red obstruction lights round the top of something tall, blinking. */
static void obstruction(Mesh *m, const BGraph *g, int si, float cx, float cz, float r, float y) {
    uint8_t ph = (uint8_t)(hash_combine(g->seed, (uint32_t)si) & 255u);
    for (int k = 0; k < 3; ++k) {
        float a = 2.0f * PI_F * (float)k / 3.0f;
        emit_emissive_box(m, v3(cx + cosf(a) * (r + 0.2f), y, cz + sinf(a) * (r + 0.2f)), 0.25f, 2, 1, ph);
    }
}

/* ---------------------------------------------------------------------------
 * Units
 * ------------------------------------------------------------------------- */

/* A vessel's shell: in painted bands (the gothic refinery's orange, cream and red), or one coat with hoops. */
static void shell(Mesh *m, const BGraph *g, const BSection *s, float cx, float cz, float r, float y0, float y1, Rng *rr) {
    int n = facets(r);
    if (!s->bands) {
        frustum(m, cx, cz, r, r, y0, y1, n, 1.0f, imat(IND_PAINT, s->paint, IP_HOOPS | grime_of(g), g->seed + s->unit));
        return;
    }
    static const int seq[3] = { PA_ORANGE, PA_CREAM, PA_RED };
    int k = s->paint == PA_ORANGE ? 0 : s->paint == PA_CREAM ? 1 : 2;
    for (float y = y0; y < y1 - 0.01f; ++k) {
        float h = minf(rng_range(rr, 1.5f, 3.5f), y1 - y);
        if (y1 - y - h < 0.8f) h = y1 - y;
        frustum(m, cx, cz, r, r, y, y + h, n, 1.0f, imat(IND_PAINT, seq[k % 3], 0, g->seed + (uint32_t)k));
        y += h;
    }
}

static void head(Mesh *m, const BGraph *g, const BSection *s, float cx, float cz, float r, float top, Mat mt) {
    int n = facets(r);
    float hh = bgen_head_height(s->head, r);
    switch (s->head) {
    case IH_DOME: dome(m, cx, cz, r, top, hh, n, mt); break;
    case IH_CONE: frustum(m, cx, cz, r, 0.06f * r, top, top + hh, n, 1.0f, mt); break;
    case IH_COWL:
        frustum(m, cx, cz, r * 1.25f, r * 1.25f, top + hh * 0.35f, top + hh, n, 1.0f, mt);
        ring(m, cx, cz, r, r * 1.25f, top + hh * 0.35f, 0, n, mt);
        ring(m, cx, cz, 0.0f, r * 1.25f, top + hh, 1, n, mt);
        for (int k = 0; k < 4; ++k) {   /* the cowl's stays */
            float a = PI_F * 0.5f * (float)k + 0.4f;
            emit_beam(m, v3(cx + cosf(a) * r, top, cz + sinf(a) * r), v3(cx + cosf(a) * r * 1.2f, top + hh * 0.4f, cz + sinf(a) * r * 1.2f), 0.12f, mt);
        }
        break;
    default:
        ring(m, cx, cz, 0.0f, r, top, 1, n, mt);
        if (s->unit == IU_TANK) frustum(m, cx, cz, r, r, top, top + 0.5f, n, 1.0f, imat(IND_MESH, steel_of(g), 0, g->seed));   /* the roof's handrail */
        else frustum(m, cx, cz, r + 0.12f, r + 0.12f, top - 0.3f, top + 0.1f, n, 1.0f, mt);                                     /* a lip */
        break;
    }
    (void)g;
}

static void vessel(Mesh *m, const BGraph *g, int si, Rng *rr) {
    const BSection *s = &g->sec[si];
    float cx, cz, r;
    bloop_circle(&s->foot.outer, &cx, &cz, &r);
    float hh = bgen_head_height(s->head, r), top = s->y1 - hh;
    frustum(m, cx, cz, r + 0.3f, r + 0.3f, G0, G0 + 0.35f, facets(r), 1.0f, concrete(g));   /* the plinth */
    ring(m, cx, cz, r, r + 0.3f, G0 + 0.35f, 1, facets(r), concrete(g));
    shell(m, g, s, cx, cz, r, G0 + 0.35f, top, rr);
    head(m, g, s, cx, cz, r, top, imat(IND_PAINT, s->bands ? PA_CREAM : s->paint, grime_of(g), g->seed + 7u));
    if (s->unit != IU_TANK) {
        /* a platform round the top of a column, its rail */
        if (s->unit == IU_COLUMN && top - G0 > 8.0f) {
            float py = top - 1.2f;
            Mat gr = imat(IND_GRATING, steel_of(g), 0, g->seed);
            ring(m, cx, cz, r, r + 0.9f, py, 1, facets(r + 0.9f), gr);
            ring(m, cx, cz, r, r + 0.9f, py - 0.08f, 0, facets(r + 0.9f), gr);
            frustum(m, cx, cz, r + 0.9f, r + 0.9f, py, py + 1.0f, facets(r + 0.9f), 1.0f, imat(IND_MESH, steel_of(g), 0, g->seed));
        }
        /* manways: round hatches on the shell */
        int nh = s->unit == IU_DOME ? 2 : 1;
        for (int k = 0; k < nh; ++k) {
            float a = rng_range(rr, 0.0f, 2.0f * PI_F), y = G0 + rng_range(rr, 1.2f, minf(4.0f, top - G0 - 1.0f));
            vec3 o = v3(cx + cosf(a) * r, y, cz + sinf(a) * r), d = v3(cosf(a), 0.0f, sinf(a));
            tube(m, o, v3_add(o, v3_scale(d, 0.35f)), 0.45f, 10, 1, imat(IND_PAINT, s->bands ? PA_SOOT : PA_STEEL, 0, g->seed));
        }
    }
    if (s->y1 - G0 > 45.0f) obstruction(m, g, si, cx, cz, r * 0.5f, s->y1 + 0.3f);
}

static void exchanger(Mesh *m, const BGraph *g, int si) {
    const BSection *s = &g->sec[si];
    float x0, z0, x1, z1;
    box_of(&s->foot.outer, &x0, &z0, &x1, &z1);
    int alongX = x1 - x0 >= z1 - z0;
    float d = (alongX ? z1 - z0 : x1 - x0) - 0.6f, r = d * 0.5f, cy = s->y1 - r;
    float cx = (x0 + x1) * 0.5f, cz = (z0 + z1) * 0.5f, L = alongX ? x1 - x0 : z1 - z0;
    vec3 ax = alongX ? v3(1, 0, 0) : v3(0, 0, 1);
    vec3 a = v3_add(v3(cx, cy, cz), v3_scale(ax, -L * 0.5f + 0.2f)), b = v3_add(v3(cx, cy, cz), v3_scale(ax, L * 0.5f - 0.6f));
    tube(m, a, b, r, 12, 1, imat(IND_PAINT, s->paint, IP_HOOPS | grime_of(g), g->seed + 3u));
    /* the channel head, a little wider, and the saddles */
    tube(m, b, v3_add(b, v3_scale(ax, 0.55f)), r + 0.12f, 12, 1, imat(IND_PAINT, steel_of(g), 0, g->seed));
    for (int k = 0; k < 2; ++k) {
        float t = k ? 0.25f : -0.25f;
        float sx = cx + (alongX ? t * L : 0.0f), sz = cz + (alongX ? 0.0f : t * L);
        float hw = alongX ? 0.3f : r * 0.8f, hd = alongX ? r * 0.8f : 0.3f;
        emit_box(m, v3(sx - hw, G0, sz - hd), v3(sx + hw, cy - r * 0.6f, sz + hd), F_NOBOT, concrete(g), UV_FACADE);
    }
}

static void stack(Mesh *m, const BGraph *g, int si, Rng *rr) {
    const BSection *s = &g->sec[si];
    float cx, cz, r0;
    bloop_circle(&s->foot.outer, &cx, &cz, &r0);
    float r1 = r0 * 0.68f, hh = bgen_head_height(s->head, r1), top = s->y1 - hh, h = top - G0;
    int n = facets(r0);
    frustum(m, cx, cz, r0 + 0.5f, r0 + 0.5f, G0, G0 + 0.5f, n, 1.0f, concrete(g));
    ring(m, cx, cz, r0, r0 + 0.5f, G0 + 0.5f, 1, n, concrete(g));
    int bands = g->style == PST_GAS ? 3 : 0;             /* red and white marker bands near the top */
    float bh = maxf(2.0f, h * 0.045f), yb = top - (float)bands * bh;
    Mat body = imat(IND_PAINT, s->paint, (g->style == PST_GAS ? 0 : IP_HOOPS) | IP_GRIME, g->seed + 11u);
    frustum(m, cx, cz, r0, lerpf(r0, r1, (yb - G0 - 0.5f) / maxf(h - 0.5f, 1.0f)), G0 + 0.5f, yb, n, 1.0f, body);
    for (int k = 0; k < bands; ++k) {
        float ya = yb + (float)k * bh, yc = ya + bh;
        float ra = lerpf(r0, r1, (ya - G0 - 0.5f) / maxf(h - 0.5f, 1.0f)), rc = lerpf(r0, r1, (yc - G0 - 0.5f) / maxf(h - 0.5f, 1.0f));
        frustum(m, cx, cz, ra, rc, ya, yc, n, 1.0f, imat(IND_PAINT, (k & 1) ? PA_WHITE : PA_RED, 0, g->seed));
    }
    Mat top_ = imat(IND_PAINT, g->style == PST_GOTHIC ? PA_RUST : PA_SOOT, IP_GRIME, g->seed);
    if (s->head == IH_COWL) head(m, g, s, cx, cz, r1, top, top_);
    else {
        frustum(m, cx, cz, r1 + 0.15f, r1 + 0.15f, top - 0.6f, top, n, 1.0f, top_);
        ring(m, cx, cz, r1 - 0.25f, r1 + 0.15f, top, 1, n, top_);
        frustum(m, cx, cz, r1 - 0.25f, r1 - 0.25f, top - 2.0f, top, n, -1.0f, top_);   /* the flue's soot-black throat */
        ring(m, cx, cz, 0.0f, r1 - 0.25f, top - 2.0f, 1, n, mat(MAT_METAL, g->seed, 4, 0, 0));
    }
    /* a plume off the night refinery's stacks (smoke) and some of the gas plants' (steam) */
    if (g->style == PST_NIGHT || (g->style == PST_GAS && rng_float(rr) < 0.5f)) {
        uint32_t seed = hash_combine(g->seed, (uint32_t)si) ^ rng_u32(rr);
        int smoke = g->style == PST_NIGHT;
        float rise = smoke ? 55.0f + r1 * 12.0f : 22.0f + r1 * 8.0f;   /* steam evaporates; smoke drifts on and thins */
        push_plume(m, v3(cx, s->y1, cz), r1 - 0.25f, rise, seed, smoke ? PLUME_SMOKE : PLUME_STEAM);
    }
    if (s->y1 - G0 > 45.0f) obstruction(m, g, si, cx, cz, r1, s->y1 + 0.4f);
}

static void flare(Mesh *m, const BGraph *g, int si, Rng *rr) {
    const BSection *s = &g->sec[si];
    const BLoop *l = &s->foot.outer;
    float cx, cz, R;
    bloop_circle(l, &cx, &cz, &R);
    float h = s->y1 - G0, tipY = s->y1 - 3.0f, rt = maxf(0.45f, R * 0.12f);
    Mat st = imat(IND_PAINT, steel_of(g), grime_of(g), g->seed);
    vec3 base[3], top[3];
    for (int k = 0; k < 3; ++k) {
        float dx = l->x[k] - cx, dz = l->z[k] - cz;
        base[k] = v3(l->x[k], G0, l->z[k]);
        top[k] = v3(cx + dx * rt / R, tipY, cz + dz * rt / R);
        emit_beam(m, base[k], top[k], maxf(0.3f, h * 0.004f), st);
        emit_box(m, v3(l->x[k] - 0.5f, G0, l->z[k] - 0.5f), v3(l->x[k] + 0.5f, G0 + 0.4f, l->z[k] + 0.5f), F_NOBOT, concrete(g), UV_FACADE);
    }
    /* braced in levels, with zig-zag diagonals on each face */
    float step = maxf(3.0f, h / 14.0f);
    int levels = (int)(h / step);
    for (int lv = 1; lv <= levels; ++lv) {
        float t0 = (float)(lv - 1) / (float)levels, t1 = (float)lv / (float)levels;
        for (int k = 0; k < 3; ++k) {
            int j = (k + 1) % 3;
            vec3 a1 = v3_add(base[k], v3_scale(v3_sub(top[k], base[k]), t1)), b1 = v3_add(base[j], v3_scale(v3_sub(top[j], base[j]), t1));
            vec3 a0 = v3_add(base[k], v3_scale(v3_sub(top[k], base[k]), t0));
            emit_beam(m, a1, b1, 0.14f, st);
            emit_beam(m, (lv & 1) ? a0 : b1, (lv & 1) ? b1 : a0, 0.1f, st);
        }
    }
    /* the riser up the middle, the tip and its flame */
    tube(m, v3(cx, G0, cz), v3(cx, tipY, cz), 0.3f, 8, 0, imat(IND_PAINT, pipe_of(g), 0, g->seed));
    tube(m, v3(cx, tipY, cz), v3(cx, s->y1 - 0.4f, cz), 0.42f, 10, 1, imat(IND_PAINT, PA_RUST, IP_GRIME, g->seed));
    float reach = flare_reach(h);
    push_flame(m, v3(cx, s->y1 - 0.45f, cz), reach, reach * 0.2f, flare_id(g, si));   /* its pilot is the only light at the tip */
    (void)rr;
    obstruction(m, g, si, cx, cz, rt, tipY - 1.0f);
}

static void gasholder(Mesh *m, const BGraph *g, int si) {
    const BSection *s = &g->sec[si];
    float cx, cz, r;
    bloop_circle(&s->foot.outer, &cx, &cz, &r);
    float h = s->y1 - G0;
    int n = s->posts > 2 ? s->posts : 3, nf = facets(r);
    Mat frame = imat(IND_PAINT, s->paint, grime_of(g), g->seed), lift = imat(IND_PAINT, g->style == PST_GAS ? PA_PALE : g->style == PST_GOTHIC ? PA_SOOT : PA_RUST, IP_HOOPS | IP_GRIME, g->seed);
    /* the water tank, then the lifts telescoped up to the fill */
    float wt = h * 0.22f;
    frustum(m, cx, cz, r - 0.8f, r - 0.8f, G0, G0 + wt, nf, 1.0f, imat(IND_PAINT, PA_SOOT, IP_RIBS | IP_GRIME, g->seed));
    float raised = wt + (h * 0.95f - wt) * s->fill, y = G0 + wt;
    for (int k = 1; k <= 3; ++k) {
        float rk = r - 0.8f - 0.45f * (float)k, yk = G0 + wt + (raised - wt) * (float)k / 3.0f;
        if (yk > y + 0.1f) frustum(m, cx, cz, rk, rk, y - 0.3f, yk, nf, 1.0f, lift);
        y = maxf(y, yk);
        if (k == 3) dome(m, cx, cz, rk, y, maxf(0.6f, rk * 0.08f), nf, lift);
        else ring(m, cx, cz, rk - 0.45f, rk, y, 1, nf, lift);
    }
    /* the guide frame: posts, rings at three heights, crosses in the top tier */
    for (int k = 0; k < n; ++k) {
        float a0 = 2.0f * PI_F * (float)k / (float)n, a1 = 2.0f * PI_F * (float)(k + 1) / (float)n;
        vec3 p0 = v3(cx + cosf(a0) * r, G0, cz + sinf(a0) * r), p1 = v3(cx + cosf(a1) * r, G0, cz + sinf(a1) * r);
        emit_beam(m, p0, v3(p0.x, s->y1, p0.z), 0.6f, frame);
        for (int lv = 1; lv <= 3; ++lv) {
            float yl = G0 + h * (float)lv / 3.0f - 0.3f;
            emit_beam(m, v3(p0.x, yl, p0.z), v3(p1.x, yl, p1.z), 0.35f, frame);
        }
        float ya = G0 + h * 2.0f / 3.0f, yb = s->y1 - 0.3f;
        emit_beam(m, v3(p0.x, ya, p0.z), v3(p1.x, yb, p1.z), 0.18f, frame);
        emit_beam(m, v3(p1.x, ya, p1.z), v3(p0.x, yb, p0.z), 0.18f, frame);
    }
}

/* The column a frame braces. */
static int braced(const BGraph *g, int fi) {
    for (int i = 0; i < g->nsec; ++i) if (g->sec[i].braceBy == fi + 1) return i;
    return -1;
}

static void frame(Mesh *m, const BGraph *g, int si) {
    const BSection *s = &g->sec[si];
    float x0, z0, x1, z1;
    box_of(&s->foot.outer, &x0, &z0, &x1, &z1);
    x0 += 0.25f; z0 += 0.25f; x1 -= 0.25f; z1 -= 0.25f;
    int ci = braced(g, si);
    float cx = (x0 + x1) * 0.5f, cz = (z0 + z1) * 0.5f, cr = 1.0f;
    if (ci >= 0) bloop_circle(&g->sec[ci].foot.outer, &cx, &cz, &cr);
    Mat st = imat(IND_PAINT, steel_of(g), grime_of(g), g->seed), gr = imat(IND_GRATING, steel_of(g), 0, g->seed);
    int levels = (int)floorf((s->y1 - G0) / 6.0f + 0.01f);
    vec3 c[4] = { v3(x0, 0, z0), v3(x1, 0, z0), v3(x1, 0, z1), v3(x0, 0, z1) };
    for (int lv = 1; lv <= levels; ++lv) {
        float y = G0 + (float)lv * 6.0f, yp = y - 6.0f;
        for (int k = 0; k < 4; ++k) {
            vec3 a = c[k], b = c[(k + 1) & 3];
            emit_beam(m, v3(a.x, y, a.z), v3(b.x, y, b.z), 0.3f, st);
            if ((k & 1) == (lv & 1)) {   /* cross-bracing on alternate faces */
                emit_beam(m, v3(a.x, yp, a.z), v3(b.x, y, b.z), 0.14f, st);
                emit_beam(m, v3(b.x, yp, b.z), v3(a.x, y, a.z), 0.14f, st);
            }
        }
        /* the level's platform round the column, and its rail */
        float hr = cr + 0.3f;
        float strips[4][4] = { { x0, z0, x1, cz - hr }, { x0, cz + hr, x1, z1 }, { x0, cz - hr, cx - hr, cz + hr }, { cx + hr, cz - hr, x1, cz + hr } };
        for (int k = 0; k < 4; ++k)
            if (strips[k][2] - strips[k][0] > 0.1f && strips[k][3] - strips[k][1] > 0.1f)
                emit_box(m, v3(strips[k][0], y + 0.15f, strips[k][1]), v3(strips[k][2], y + 0.22f, strips[k][3]), F_TOP | F_BOT, gr, UV_UNIT);
        for (int k = 0; k < 4; ++k) railing(m, g, v3(c[k].x, y + 0.22f, c[k].z), v3(c[(k + 1) & 3].x, y + 0.22f, c[(k + 1) & 3].z), 1.0f);
        /* ties to the column */
        if (ci >= 0)
            for (int k = 0; k < 4; ++k)
                emit_beam(m, v3(c[k].x, y - 0.4f, c[k].z), v3(cx + (c[k].x - cx) * cr / (fabsf(c[k].x - cx) * 1.414f), y - 0.4f, cz + (c[k].z - cz) * cr / (fabsf(c[k].z - cz) * 1.414f)), 0.2f, st);
    }
}

static void rack(Mesh *m, const BGraph *g, int si, Rng *rr) {
    const BSection *s = &g->sec[si];
    float x0, z0, x1, z1;
    box_of(&s->foot.outer, &x0, &z0, &x1, &z1);
    int alongX = x1 - x0 >= z1 - z0;
    Mat st = imat(IND_PAINT, steel_of(g), grime_of(g), g->seed);
    float tiers[2] = { s->y1 - 0.3f, s->y1 - 2.3f };
    int nt = s->y1 - G0 > 7.0f ? 2 : 1;
    /* the bents: cross beams between each pair of legs; the long beams down both sides */
    vec3 first[2] = { v3(0, 0, 0), v3(0, 0, 0) }, last[2] = { v3(0, 0, 0), v3(0, 0, 0) };
    int bent = 0;
    for (int k = 0; k + 1 < g->nsup; ++k) {
        const BSupport *a = &g->sup[k], *b = &g->sup[k + 1];
        if (a->section != si || b->section != si) continue;
        for (int t = 0; t < nt; ++t) emit_beam(m, v3(a->x0, tiers[t], a->z0), v3(b->x0, tiers[t], b->z0), 0.32f, st);
        if (!bent) { first[0] = v3(a->x0, 0, a->z0); first[1] = v3(b->x0, 0, b->z0); }
        last[0] = v3(a->x0, 0, a->z0); last[1] = v3(b->x0, 0, b->z0);
        bent++;
        ++k;
    }
    for (int sd = 0; sd < 2 && bent > 1; ++sd)
        for (int t = 0; t < nt; ++t) emit_beam(m, v3(first[sd].x, tiers[t], first[sd].z), v3(last[sd].x, tiers[t], last[sd].z), 0.24f, st);
    /* the pipes it carries */
    float w = alongX ? z1 - z0 : x1 - x0, mid = alongX ? (z0 + z1) * 0.5f : (x0 + x1) * 0.5f;
    float a0 = (alongX ? x0 : z0) + 0.2f, a1 = (alongX ? x1 : z1) - 0.2f;
    for (int t = 0; t < nt; ++t) {
        float b = mid - w * 0.5f + 0.5f;
        while (b < mid + w * 0.5f - 0.5f) {
            float r = rng_range(rr, 0.12f, 0.42f);
            if (b + r > mid + w * 0.5f - 0.3f) break;
            float y = tiers[t] + 0.16f + r;
            int insulated = rng_float(rr) < 0.3f;
            Mat pm = imat(IND_PAINT, insulated ? PA_CLAD : pipe_of(g), insulated ? IP_RIBS : grime_of(g), g->seed + (uint32_t)(b * 10.0f));
            vec3 pa = alongX ? v3(a0, y, b + r) : v3(b + r, y, a0), pb = alongX ? v3(a1, y, b + r) : v3(b + r, y, a1);
            tube(m, pa, pb, r, r > 0.25f ? 10 : 8, 1, pm);
            b += 2.0f * r + rng_range(rr, 0.15f, 0.5f);
        }
    }
}

/* x-range of convex loop l along the line at z. */
static int span_at(const BLoop *l, float z, float *lo, float *hi) {
    *lo = 1e30f; *hi = -1e30f;
    for (int i = 0; i < l->n; ++i) {
        int j = (i + 1) % l->n;
        float za = l->z[i], zb = l->z[j];
        if ((za - z) * (zb - z) > 0.0f || fabsf(zb - za) < 1e-6f) continue;
        float x = l->x[i] + (l->x[j] - l->x[i]) * (z - za) / (zb - za);
        *lo = minf(*lo, x); *hi = maxf(*hi, x);
    }
    return *lo < *hi;
}

/* A deck's plate at y (convex outline, round holes), cut in 0.5 m strips. */
static void plate(Mesh *m, const BGraph *g, int di, const BLoop *l, float y, int up, Mat mt) {
    float x0, z0, x1, z1;
    box_of(l, &x0, &z0, &x1, &z1);
    int hs[BG_MAX_HOLE], nh = 0;
    for (int h = 0; h < g->nhole; ++h) if (g->hole[h].deck == di) hs[nh++] = h;
    for (int i = 1; i < nh; ++i)
        for (int j = i; j > 0 && g->hole[hs[j]].x < g->hole[hs[j - 1]].x; --j) { int t = hs[j]; hs[j] = hs[j - 1]; hs[j - 1] = t; }
    vec3 nn = v3(0, up ? 1.0f : -1.0f, 0);
    for (float za = z0; za < z1 - 1e-3f; za += 0.5f) {
        float zb = minf(za + 0.5f, z1);
        float La, Ra, Lb, Rb;
        if (!span_at(l, za + 1e-3f, &La, &Ra) || !span_at(l, zb - 1e-3f, &Lb, &Rb)) continue;
        float ca[2 * BG_MAX_HOLE + 2], cb[2 * BG_MAX_HOLE + 2];
        int nc = 0;
        ca[nc] = La; cb[nc] = Lb; nc++;
        for (int k = 0; k < nh; ++k) {
            const BHole *h = &g->hole[hs[k]];
            if (h->z + h->r < za || h->z - h->r > zb) continue;
            float wa = sqrtf(maxf(0.0f, h->r * h->r - (za - h->z) * (za - h->z))), wb = sqrtf(maxf(0.0f, h->r * h->r - (zb - h->z) * (zb - h->z)));
            ca[nc] = h->x - wa; cb[nc] = h->x - wb; nc++;
            ca[nc] = h->x + wa; cb[nc] = h->x + wb; nc++;
        }
        ca[nc] = Ra; cb[nc] = Rb; nc++;
        for (int k = 0; k + 1 < nc; k += 2) {
            float al = ca[k], ar = maxf(ca[k + 1], al), bl = cb[k], br = maxf(cb[k + 1], bl);
            if (ar - al < 1e-3f && br - bl < 1e-3f) continue;
            float uv[4][2] = { { al, za }, { ar, za }, { br, zb }, { bl, zb } };
            emit_quad(m, v3(al, y, za), v3(ar, y, za), v3(br, y, zb), v3(bl, y, zb), nn, uv, mt);
        }
    }
}

static void deck(Mesh *m, const BGraph *g, int di, Rng *rr) {
    const BSection *s = &g->sec[di];
    const BLoop *l = &s->foot.outer;
    Mat top = imat(IND_GRATING, g->style == PST_GOTHIC ? PA_CREAM : steel_of(g), 0, g->seed);
    Mat girder = imat(IND_PAINT, g->style == PST_GOTHIC ? PA_SOOT : steel_of(g), grime_of(g), g->seed);
    plate(m, g, di, l, s->y1, 1, top);
    plate(m, g, di, l, s->y0, 0, girder);
    for (int k = 0; k < l->n; ++k) {
        int k1 = (k + 1) % l->n;
        float ex = l->x[k1] - l->x[k], ez = l->z[k1] - l->z[k], len = sqrtf(ex * ex + ez * ez);
        if (len < 1e-3f) continue;
        float uv[4][2] = { { 0, s->y0 }, { len, s->y0 }, { len, s->y1 }, { 0, s->y1 } };
        vec3 nn = v3(-ez / len, 0, ex / len);
        emit_quad(m, v3(l->x[k], s->y0, l->z[k]), v3(l->x[k1], s->y0, l->z[k1]), v3(l->x[k1], s->y1, l->z[k1]), v3(l->x[k], s->y1, l->z[k]), nn, uv, girder);
        railing(m, g, v3(l->x[k], s->y1, l->z[k]), v3(l->x[k1], s->y1, l->z[k1]), 1.1f);
    }
    /* the holes: their edge through the plate, and the collar on each vessel */
    for (int h = 0; h < g->nhole; ++h) {
        const BHole *hl = &g->hole[h];
        if (hl->deck != di) continue;
        float vx, vz, vr;
        bloop_circle(&g->sec[hl->unit].foot.outer, &vx, &vz, &vr);
        frustum(m, hl->x, hl->z, hl->r, hl->r, s->y0, s->y1, facets(hl->r), -1.0f, girder);
        frustum(m, vx, vz, vr + 0.25f, vr + 0.25f, s->y0 - 0.7f, s->y0 + 0.2f, facets(vr), 1.0f, girder);
        ring(m, vx, vz, vr, vr + 0.25f, s->y0 + 0.2f, 1, facets(vr), girder);
        ring(m, vx, vz, vr, vr + 0.25f, s->y0 - 0.7f, 0, facets(vr), girder);
        for (int k = 0; k < 4; ++k) {   /* brackets out under the plate */
            float a = PI_F * 0.5f * (float)k + PI_F * 0.25f;
            emit_beam(m, v3(vx + cosf(a) * (vr + 0.2f), s->y0 - 0.6f, vz + sinf(a) * (vr + 0.2f)),
                      v3(vx + cosf(a) * (hl->r + 0.6f), s->y0, vz + sinf(a) * (hl->r + 0.6f)), 0.18f, girder);
        }
    }
    /* the gothic refinery's hoist and claw, hanging beneath */
    if (g->style == PST_GOTHIC) {
        float x0, z0, x1, z1;
        box_of(l, &x0, &z0, &x1, &z1);
        float hx = rng_range(rr, x0 + 2.0f, x1 - 2.0f), hz = rng_range(rr, z0 + 2.0f, z1 - 2.0f), drop = minf(4.5f, s->y0 - G0 - 2.5f);
        int clear = drop > 1.0f;
        for (int i = 0; i < g->nsec && clear; ++i)
            if (i != di && g->sec[i].y1 > s->y0 - drop - 1.5f && g->sec[i].y0 < s->y0 && bfoot_boundary_dist(&g->sec[i].foot, hx, hz, NULL, NULL) < 1.5f + (bfoot_contains(&g->sec[i].foot, hx, hz) ? 1e3f : 0.0f)) clear = 0;
        if (clear) {
            Mat dark = imat(IND_PAINT, PA_SOOT, IP_GRIME, g->seed);
            emit_box(m, v3(hx - 0.7f, s->y0 - 0.8f, hz - 0.5f), v3(hx + 0.7f, s->y0, hz + 0.5f), F_NOBOT | F_BOT, imat(IND_PAINT, PA_ORANGE, 0, g->seed), UV_FACADE);
            tube(m, v3(hx, s->y0 - 0.8f, hz), v3(hx, s->y0 - drop, hz), 0.04f, 4, 0, dark);
            emit_box(m, v3(hx - 0.3f, s->y0 - drop - 0.4f, hz - 0.3f), v3(hx + 0.3f, s->y0 - drop, hz + 0.3f), F_NOBOT | F_BOT, dark, UV_FACADE);
            for (int k = 0; k < 3; ++k) {
                float a = 2.0f * PI_F * (float)k / 3.0f;
                vec3 p = v3(hx, s->y0 - drop - 0.3f, hz), q = v3(hx + cosf(a) * 0.8f, s->y0 - drop - 1.0f, hz + sinf(a) * 0.8f);
                emit_beam(m, p, q, 0.12f, dark);
                emit_beam(m, q, v3(hx + cosf(a) * 0.45f, s->y0 - drop - 1.6f, hz + sinf(a) * 0.45f), 0.1f, dark);
            }
        }
    }
}

static void transformer(Mesh *m, const BGraph *g, int si) {
    const BSection *s = &g->sec[si];
    float x0, z0, x1, z1;
    box_of(&s->foot.outer, &x0, &z0, &x1, &z1);
    float mg = s->fill, th = s->y1 - G0 - 1.6f;
    Mat con = concrete(g), tank = imat(IND_PAINT, s->paint, IP_GRIME, g->seed), fin = imat(IND_PAINT, s->paint, IP_RIBS, g->seed + 1u);
    /* the bund: a low wall round a gravel floor */
    emit_box(m, v3(x0, G0, z0), v3(x1, G0 + 0.8f, z0 + 0.25f), F_NOBOT, con, UV_FACADE);
    emit_box(m, v3(x0, G0, z1 - 0.25f), v3(x1, G0 + 0.8f, z1), F_NOBOT, con, UV_FACADE);
    emit_box(m, v3(x0, G0, z0 + 0.25f), v3(x0 + 0.25f, G0 + 0.8f, z1 - 0.25f), F_NOBOT, con, UV_FACADE);
    emit_box(m, v3(x1 - 0.25f, G0, z0 + 0.25f), v3(x1, G0 + 0.8f, z1 - 0.25f), F_NOBOT, con, UV_FACADE);
    emit_box(m, v3(x0 + 0.25f, G0, z0 + 0.25f), v3(x1 - 0.25f, G0 + 0.12f, z1 - 0.25f), F_TOP, mat(MAT_METAL, g->seed, 0, 0, 0), UV_FACADE);
    float tx0 = x0 + mg, tz0 = z0 + mg, tx1 = x1 - mg, tz1 = z1 - mg;
    emit_box(m, v3(tx0, G0 + 0.3f, tz0), v3(tx1, G0 + th, tz1), F_NOBOT, tank, UV_FACADE);
    emit_box(m, v3(tx0 - 0.1f, G0 + 0.12f, tz0 - 0.1f), v3(tx1 + 0.1f, G0 + 0.3f, tz1 + 0.1f), F_NOBOT, con, UV_FACADE);
    /* radiator banks down the long sides */
    int alongX = tx1 - tx0 >= tz1 - tz0;
    float L = alongX ? tx1 - tx0 : tz1 - tz0;
    int nfin = (int)(L / 0.45f);
    for (int sd = 0; sd < 2; ++sd)
        for (int k = 1; k < nfin; ++k) {
            float u = (alongX ? tx0 : tz0) + L * (float)k / (float)nfin;
            float o0 = sd ? (alongX ? tz1 : tx1) : (alongX ? tz0 - 0.9f : tx0 - 0.9f), o1 = o0 + 0.9f;
            if (alongX) emit_box(m, v3(u - 0.03f, G0 + 0.5f, o0), v3(u + 0.03f, G0 + th - 0.3f, o1), F_SIDES | F_TOP, fin, UV_FACADE);
            else        emit_box(m, v3(o0, G0 + 0.5f, u - 0.03f), v3(o1, G0 + th - 0.3f, u + 0.03f), F_SIDES | F_TOP, fin, UV_FACADE);
        }
    /* the conservator along the top, and a bushing under each phase */
    float cy = G0 + th + 0.75f, cxm = (tx0 + tx1) * 0.5f, czm = (tz0 + tz1) * 0.5f;
    vec3 ca = alongX ? v3(tx0 + 0.3f, cy, tz0 + 0.5f) : v3(tx0 + 0.5f, cy, tz0 + 0.3f), cb = alongX ? v3(tx1 - 0.3f, cy, tz0 + 0.5f) : v3(tx0 + 0.5f, cy, tz1 - 0.3f);
    tube(m, ca, cb, 0.32f, 10, 1, tank);
    emit_beam(m, v3_add(ca, v3(0, -0.75f, 0)), v3_add(ca, v3(0.0f, -0.2f, 0.0f)), 0.12f, tank);
    emit_beam(m, v3_add(cb, v3(0, -0.75f, 0)), v3_add(cb, v3(0.0f, -0.2f, 0.0f)), 0.12f, tank);
    (void)cxm; (void)czm;
    for (int p = 0; p < g->npipe; ++p) {
        const BPipe *c = &g->pipe[p];
        if (c->kind != PIPE_CONDUCTOR || c->a != si) continue;
        tube(m, v3(c->x[0], G0 + th, c->z[0]), v3(c->x[0], c->y[0], c->z[0]), 0.13f, 8, 1, imat(IND_PAINT, PA_BRASS, IP_HOOPS, g->seed));
    }
}

static void gantry(Mesh *m, const BGraph *g, int si) {
    const BSection *s = &g->sec[si];
    Mat st = imat(IND_PAINT, PA_GALV, 0, g->seed);
    float y = s->y1 - 0.4f;
    vec3 first[2] = { v3(0, 0, 0), v3(0, 0, 0) }, last[2] = { v3(0, 0, 0), v3(0, 0, 0) };
    int portal = 0;
    for (int k = 0; k + 1 < g->nsup; ++k) {
        const BSupport *a = &g->sup[k], *b = &g->sup[k + 1];
        if (a->section != si || b->section != si) continue;
        emit_beam(m, v3(a->x0, y, a->z0), v3(b->x0, y, b->z0), 0.35f, st);
        emit_beam(m, v3(a->x0, y - 1.6f, a->z0), v3(b->x0, y, b->z0), 0.12f, st);
        if (!portal) { first[0] = v3(a->x0, 0, a->z0); first[1] = v3(b->x0, 0, b->z0); }
        last[0] = v3(a->x0, 0, a->z0); last[1] = v3(b->x0, 0, b->z0);
        portal++;
        ++k;
    }
    for (int sd = 0; sd < 2 && portal > 1; ++sd) {
        emit_beam(m, v3(first[sd].x, y, first[sd].z), v3(last[sd].x, y, last[sd].z), 0.4f, st);
        emit_beam(m, v3(first[sd].x, y - 1.2f, first[sd].z), v3(last[sd].x, y - 1.2f, last[sd].z), 0.2f, st);
    }
    /* insulator strings where the conductors come in */
    for (int p = 0; p < g->npipe; ++p) {
        const BPipe *c = &g->pipe[p];
        if (c->kind != PIPE_CONDUCTOR || c->b != si) continue;
        tube(m, v3(c->x[1], c->y[1], c->z[1]), v3(c->x[1], y - 0.2f, c->z[1]), 0.1f, 6, 1, imat(IND_PAINT, PA_BRASS, IP_HOOPS, g->seed));
    }
}

/* ---------------------------------------------------------------------------
 * Pipes, supports, ladders, stairs, the fence
 * ------------------------------------------------------------------------- */

static void pipes(Mesh *m, const BGraph *g) {
    for (int k = 0; k < g->npipe; ++k) {
        const BPipe *p = &g->pipe[k];
        if (p->kind == PIPE_CONDUCTOR) {
            Mat wire = mat(MAT_METAL, g->seed, 2, 0, 0);
            tube(m, v3(p->x[0], p->y[0], p->z[0]), v3(p->x[1], p->y[1], p->z[1]), 0.035f, 4, 0, wire);
            continue;
        }
        uint32_t h = hash_combine(g->seed ^ 0x919Eu, (uint32_t)k);
        int insulated = hash_float(h) < 0.25f;
        Mat pm = imat(IND_PAINT, insulated ? PA_CLAD : pipe_of(g), insulated ? IP_RIBS : grime_of(g), h);
        int n = p->r > 0.25f ? 10 : 8;
        for (int s = 0; s + 1 < p->n; ++s) {
            vec3 a = v3(p->x[s], p->y[s], p->z[s]), b = v3(p->x[s + 1], p->y[s + 1], p->z[s + 1]);
            vec3 d = v3_sub(b, a);
            float len = v3_len(d);
            if (len < 1e-3f) continue;
            d = v3_scale(d, 1.0f / len);
            /* overlap at the bends, so the elbows close */
            if (s > 0) a = v3_sub(a, v3_scale(d, p->r));
            if (s + 2 < p->n) b = v3_add(b, v3_scale(d, p->r));
            tube(m, a, b, p->r, n, 0, pm);
        }
        /* flanges where it meets its units */
        Mat fl = imat(IND_PAINT, steel_of(g), 0, h);
        for (int e = 0; e < 2; ++e) {
            int i0 = e ? p->n - 1 : 0, i1 = e ? p->n - 2 : 1;
            vec3 a = v3(p->x[i0], p->y[i0], p->z[i0]), b = v3(p->x[i1], p->y[i1], p->z[i1]);
            vec3 d = v3_sub(b, a);
            float len = v3_len(d);
            if (len < 0.2f) continue;
            tube(m, a, v3_add(a, v3_scale(d, 0.12f / len)), p->r * 1.6f, n, 1, fl);
        }
        float posts[32][4];
        int np = bgen_pipe_posts(g, p, posts, 32, NULL);
        Mat st = imat(IND_PAINT, steel_of(g), grime_of(g), g->seed);
        for (int q = 0; q < np; ++q) {
            float x = posts[q][0], z = posts[q][1], y0 = posts[q][2], y1 = posts[q][3];
            emit_box(m, v3(x - 0.1f, y0, z - 0.1f), v3(x + 0.1f, y1, z + 0.1f), F_SIDES, st, UV_FACADE);
            emit_box(m, v3(x - 0.35f, y1 - 0.12f, z - 0.35f), v3(x + 0.35f, y1, z + 0.35f), F_NOBOT | F_BOT, st, UV_FACADE);
        }
    }
}

/* Legs and posts standing on the ground. */
static void legs(Mesh *m, const BGraph *g) {
    for (int k = 0; k < g->nsup; ++k) {
        const BSupport *s = &g->sup[k];
        if (s->type != SUP_LEG) continue;
        const BSection *on = &g->sec[s->section];
        int paint = on->type == SEC_DECK && g->style == PST_GOTHIC ? PA_SOOT : on->unit == IU_GANTRY ? PA_GALV : steel_of(g);
        Mat st = imat(IND_PAINT, paint, grime_of(g), g->seed);
        float h = s->size * 0.5f;
        if (on->unit == IU_FLARE) continue;   /* the mast's own legs */
        emit_box(m, v3(s->x0 - h, s->y0, s->z0 - h), v3(s->x0 + h, s->y1, s->z0 + h), F_SIDES, st, UV_FACADE);
        emit_box(m, v3(s->x0 - h - 0.2f, G0, s->z0 - h - 0.2f), v3(s->x0 + h + 0.2f, G0 + 0.25f, s->z0 + h + 0.2f), F_NOBOT, concrete(g), UV_FACADE);
    }
}

static void ladder(Mesh *m, const BGraph *g, const BAccess *a) {
    Mat st = imat(IND_PAINT, g->style == PST_GAS ? PA_YELLOW : steel_of(g), 0, g->seed);
    Xf sv = xf_save(m);
    xf_enter(m, a->x, a->z, a->nz, -a->nx);   /* x along the wall, z out of it */
    float top = a->y1 + 1.0f, h = top - G0;
    emit_box(m, v3(-0.25f, G0, -0.03f), v3(-0.2f, top, 0.03f), F_SIDES | F_TOP, st, UV_FACADE);
    emit_box(m, v3(0.2f, G0, -0.03f), v3(0.25f, top, 0.03f), F_SIDES | F_TOP, st, UV_FACADE);
    for (float y = G0 + 0.3f; y < a->y1 + 0.05f; y += 0.3f)
        emit_box(m, v3(-0.2f, y, -0.015f), v3(0.2f, y + 0.03f, 0.015f), F_TOP | F_PZ | F_NZ, st, UV_FACADE);
    if (h > 6.0f) {   /* a cage from head height up */
        for (float y = G0 + 2.5f; y < top; y += 1.5f) {
            emit_box(m, v3(-0.36f, y, 0.0f), v3(-0.32f, y + 0.04f, 0.75f), F_SIDES | F_TOP, st, UV_FACADE);
            emit_box(m, v3(0.32f, y, 0.0f), v3(0.36f, y + 0.04f, 0.75f), F_SIDES | F_TOP, st, UV_FACADE);
            emit_box(m, v3(-0.36f, y, 0.71f), v3(0.36f, y + 0.04f, 0.75f), F_SIDES | F_TOP, st, UV_FACADE);
        }
        for (int k = -1; k <= 1; ++k)
            emit_box(m, v3((float)k * 0.3f - 0.02f, G0 + 2.5f, 0.71f), v3((float)k * 0.3f + 0.02f, top, 0.75f), F_SIDES, st, UV_FACADE);
    }
    xf_restore(m, sv);
}

static void stair(Mesh *m, const BGraph *g, const BAccess *a) {
    Mat st = imat(IND_PAINT, g->style == PST_GAS ? PA_YELLOW : steel_of(g), grime_of(g), g->seed);
    Mat gr = imat(IND_GRATING, steel_of(g), 0, g->seed);
    float rise = a->y1 - G0, run = rise * 1.15f;
    vec3 dir = v3(a->nx, 0, a->nz), side = v3(-a->nz, 0, a->nx);
    vec3 foot = v3(a->x, G0, a->z), head = v3(a->x + a->nx * run, a->y1, a->z + a->nz * run);
    for (int sd = -1; sd <= 1; sd += 2) {
        vec3 o = v3_scale(side, 0.55f * (float)sd);
        emit_beam(m, v3_add(foot, o), v3_add(head, o), 0.2f, st);
        emit_beam(m, v3_add(v3_add(foot, o), v3(0, 1.0f, 0)), v3_add(v3_add(head, o), v3(0, 1.0f, 0)), 0.05f, st);
        emit_beam(m, v3_add(foot, o), v3_add(v3_add(foot, o), v3(0, 1.0f, 0)), 0.05f, st);
    }
    int steps = (int)ceilf(rise / 0.22f);
    for (int k = 1; k < steps; ++k) {
        float t = (float)k / (float)steps;
        vec3 c = v3_add(foot, v3_scale(v3_sub(head, foot), t));
        vec3 lo = v3_sub(v3_sub(c, v3_scale(side, 0.5f)), v3_scale(dir, 0.13f)), hi = v3_add(v3_add(c, v3_scale(side, 0.5f)), v3_scale(dir, 0.13f));
        emit_box(m, v3(minf(lo.x, hi.x), c.y - 0.04f, minf(lo.z, hi.z)), v3(maxf(lo.x, hi.x), c.y, maxf(lo.z, hi.z)), F_NOBOT | F_BOT, gr, UV_UNIT);
    }
    /* a landing across to the deck */
    const BSection *d = &g->sec[a->section];
    float qx, qz;
    bfoot_boundary_dist(&d->foot, head.x, head.z, &qx, &qz);
    float lx0 = minf(head.x, qx) - 0.6f, lx1 = maxf(head.x, qx) + 0.6f, lz0 = minf(head.z, qz) - 0.6f, lz1 = maxf(head.z, qz) + 0.6f;
    emit_box(m, v3(lx0, a->y1 - 0.12f, lz0), v3(lx1, a->y1, lz1), F_NOBOT | F_BOT, gr, UV_UNIT);
}

/* Whether the fence line at (x, z) runs into one of the buildings `on` (n of them) standing on it. */
static int fence_blocked(const BGraph *g, const int *on, int n, float x, float z) {
    for (int i = 0; i < n; ++i) {
        const BFoot *f = &g->sec[on[i]].foot;
        if (bfoot_contains(f, x, z) || bfoot_boundary_dist(f, x, z, NULL, NULL) < 0.05f) return 1;
    }
    return 0;
}

static void fence_push(FenceRun *out, int *n, int max, float ax, float az, float dx, float dz, float u0, float u1, float base, int gate) {
    if (u1 - u0 < 0.05f || *n >= max) return;
    FenceRun *r = &out[(*n)++];
    r->x0 = ax + dx * u0; r->z0 = az + dz * u0; r->x1 = ax + dx * u1; r->z1 = az + dz * u1;
    r->u = base + u0; r->gate = gate;
}

int industry_fence(const BGraph *g, FenceRun *out, int max) {
    float x0 = g->fence[0], z0 = g->fence[1], x1 = g->fence[2], z1 = g->fence[3];
    /* the sides as the gate's frame runs them: S along +x, E along +z, N along -x, W along -z */
    float cx[4] = { x0, x1, x1, x0 }, cz[4] = { z0, z0, z1, z1 };
    int sides[4] = { SIDE_S, SIDE_E, SIDE_N, SIDE_W };
    int n = 0;
    float base = 0.0f;
    for (int e = 0; e < 4; ++e) {
        float ax = cx[e], az = cz[e], bx = cx[(e + 1) & 3], bz = cz[(e + 1) & 3];
        float len = fabsf(bx - ax) + fabsf(bz - az);
        if (len < 0.05f) continue;
        float dx = (bx - ax) / len, dz = (bz - az) / len;
        /* the buildings standing at street level whose boxes reach this side */
        int on[BG_MAX_SECT], non = 0;
        for (int i = 0; i < g->nsec; ++i) {
            const BSection *s = &g->sec[i];
            if (s->y0 > G0 + 0.01f || !bsec_facade(s)) continue;
            float sx0, sz0, sx1, sz1;
            box_of(&s->foot.outer, &sx0, &sz0, &sx1, &sz1);
            if (sx0 < maxf(ax, bx) + 0.1f && sx1 > minf(ax, bx) - 0.1f && sz0 < maxf(az, bz) + 0.1f && sz1 > minf(az, bz) - 0.1f) on[non++] = i;
        }
        /* where along this side the gate is, as the generator measured it (from its own frame's origin) */
        float g0 = len + 1.0f, g1 = len + 1.0f;
        if (sides[e] == g->gateSide) {
            g0 = maxf(g->gateU - g->gateW * 0.5f, 0.0f); g1 = minf(g->gateU + g->gateW * 0.5f, len);
            if (!fence_blocked(g, on, non, ax + dx * g->gateU, az + dz * g->gateU)) fence_push(out, &n, max, ax, az, dx, dz, g0, g1, base, 1);
        }
        /* the side in 5 cm steps, its open stretches either side of the gate */
        int steps = (int)ceilf(len / 0.05f), open = 0;
        float start = 0.0f;
        for (int k = 0; k <= steps; ++k) {
            float u = minf(((float)k + 0.5f) * len / (float)steps, len);
            int now = k < steps && !fence_blocked(g, on, non, ax + dx * u, az + dz * u);
            if (now == open) continue;
            float at = len * (float)k / (float)steps;
            if (now) start = at;
            else {
                fence_push(out, &n, max, ax, az, dx, dz, start, minf(at, g0), base, 0);
                fence_push(out, &n, max, ax, az, dx, dz, maxf(start, g1), at, base, 0);
            }
            open = now;
        }
        base += len;
    }
    return n;
}

/* Which material the plant's fence wears, the wavy wire net or the welded mesh (MAT_INDUSTRY params w:
 * 1 + the material; 0 is the procedural chain-link); the gate's the same. */
static int fence_kind(const BGraph *g) { return 1 + ((hash_combine(g->seed, 0xFE4Cu) & 1u) ? PBR_FENCE_WELD : PBR_FENCE_NET); }

/* The fence round the yard: a run of mesh between posts at most 3 m apart and
 * at both its ends, a rail along its top, and a sliding gate framed top and
 * bottom between heavier posts. */
static void fence(Mesh *m, const BGraph *g) {
    float h = g->archetype == ARCH_SUBSTATION ? 2.6f : g->style == PST_GOTHIC ? 3.0f : 2.4f;
    Mat st = imat(IND_PAINT, steel_of(g), 0, g->seed), mesh = mat(MAT_INDUSTRY, g->seed, IND_MESH, (uint8_t)steel_of(g), (uint8_t)fence_kind(g));
    FenceRun run[INDUSTRY_MAX_FENCE];
    int nr = industry_fence(g, run, INDUSTRY_MAX_FENCE);
    float post[INDUSTRY_MAX_FENCE * 8][2];
    int np = 0;
    for (int pass = 0; pass < 2; ++pass)   /* the gates first, so their posts win where a run meets one */
        for (int i = 0; i < nr; ++i) {
            const FenceRun *r = &run[i];
            if (r->gate != (pass == 0)) continue;
            float len = sqrtf((r->x1 - r->x0) * (r->x1 - r->x0) + (r->z1 - r->z0) * (r->z1 - r->z0));
            float y0 = G0 + (r->gate ? 0.1f : 0.0f);
            vec3 n = v3((r->z1 - r->z0) / len, 0.0f, -(r->x1 - r->x0) / len);
            float uv[4][2] = { { r->u, y0 - G0 }, { r->u + len, y0 - G0 }, { r->u + len, h }, { r->u, h } };
            vec3 a = v3(r->x0, y0, r->z0), b = v3(r->x1, y0, r->z1), c = v3(r->x1, G0 + h, r->z1), d = v3(r->x0, G0 + h, r->z0);
            emit_quad(m, a, b, c, d, n, uv, mesh);
            emit_quad(m, a, b, c, d, v3_scale(n, -1.0f), uv, mesh);
            emit_beam(m, d, c, r->gate ? 0.08f : 0.05f, st);
            if (r->gate) emit_beam(m, a, b, 0.08f, st);
            int k = r->gate ? 1 : (int)ceilf(len / 3.0f);
            for (int q = 0; q <= k; ++q) {
                float t = (float)q / (float)k, px = r->x0 + (r->x1 - r->x0) * t, pz = r->z0 + (r->z1 - r->z0) * t;
                int dup = 0;
                for (int j = 0; j < np && !dup; ++j) dup = fabsf(post[j][0] - px) < 0.05f && fabsf(post[j][1] - pz) < 0.05f;
                if (dup || np >= INDUSTRY_MAX_FENCE * 8) continue;
                post[np][0] = px; post[np][1] = pz; np++;
                float s = r->gate ? 0.08f : 0.05f;
                emit_box(m, v3(px - s, G0, pz - s), v3(px + s, G0 + h + 0.08f, pz + s), F_SIDES | F_TOP, st, UV_FACADE);
            }
        }
}

/* A neon name board over the control building's door. */
static void name_board(Mesh *m, const BGraph *g) {
    for (int d = 0; d < g->ndoor; ++d) {
        const BDoor *dr = &g->door[d];
        if (dr->type != DOOR_PRIMARY) continue;
        const BSection *s = &g->sec[dr->section];
        Xf sv = xf_save(m);
        float len = enter_face(m, &s->foot.outer, dr->face);
        Face f = face_local(len);
        float w = minf(len - 1.0f, 5.0f), y0 = G0 + dr->height + 0.4f;
        if (y0 + 0.9f < s->y1 - 0.2f) {
            uint32_t seed = hash_combine(g->seed, 0x5A3Eu);
            uint8_t col = (uint8_t)(seed & 7u);
            emit_face_panel(m, &f, dr->u - w * 0.5f, dr->u + w * 0.5f, y0, y0 + 0.8f, 0.12f,
                            mat(MAT_NEON, seed, (uint8_t)(col | (1 << 3)), q_half(w), q_half(0.8f)));
        }
        xf_restore(m, sv);
    }
}

void emit_industry(Mesh *m, const Building *b, const BGraph *g) {
    Rng r = rng_make(b->seed ^ 0x1D57u);
    for (int si = 0; si < g->nsec; ++si) {
        const BSection *s = &g->sec[si];
        if (s->type == SEC_DECK) { deck(m, g, si, &r); continue; }
        switch (s->unit) {
        case IU_COLUMN: case IU_DOME: case IU_TANK: vessel(m, g, si, &r); break;
        case IU_EXCHANGER:   exchanger(m, g, si); break;
        case IU_STACK:       stack(m, g, si, &r); break;
        case IU_FLARE:       flare(m, g, si, &r); break;
        case IU_GASHOLDER:   gasholder(m, g, si); break;
        case IU_FRAME:       frame(m, g, si); break;
        case IU_RACK:        rack(m, g, si, &r); break;
        case IU_TRANSFORMER: transformer(m, g, si); break;
        case IU_FIREWALL: {
            float x0, z0, x1, z1;
            box_of(&s->foot.outer, &x0, &z0, &x1, &z1);
            emit_box(m, v3(x0, G0, z0), v3(x1, s->y1, z1), F_NOBOT, concrete(g), UV_FACADE);
            break;
        }
        case IU_GANTRY:      gantry(m, g, si); break;
        default:             break;
        }
    }
    legs(m, g);
    pipes(m, g);
    for (int k = 0; k < g->nacc; ++k) {
        if (g->acc[k].type == ACC_STAIR) stair(m, g, &g->acc[k]);
        else ladder(m, g, &g->acc[k]);
    }
    fence(m, g);
    emit_lamps(m, g);
    name_board(m, g);
}
