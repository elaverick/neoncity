#include "city_internal.h"

/* ===========================================================================
 * Relief on a curtain wall: stone piers, floor beams, mullions and a coping
 * standing proud of the glass (the look of 'Bishopsgate.png', in the root).
 *
 * The glass stays on the footprint's line (z = 0 in the face's frame), so
 * everything behind it (interior mapping, baked interiors, real windows, the
 * prepass's cut-outs) is as it was; the relief is built out from there. The
 * layout follows the facade shader's (shaders/city/facade.glsl: whole bays
 * stretched to the face, floors counted up from the ground floor's top), so a
 * pier stands on each bay line and a beam covers each spandrel.
 *
 * One extrusion runs a pier or mullion the height of the section, whatever
 * its floors, and a beam the width of the face, whatever its bays, which keeps
 * the vertices down. Finer things are the shader's: the rail across each pane,
 * and the mullions' far stand-ins (they shrink away in city.vert). Only one
 * building in six splits its bays into lights with mullions and rails; the
 * rest show each bay as one pane between its piers.
 *
 * MAT_TRIM vertices (shadeTrim in shaders/city/facade.glsl):
 *   params y section, z kind | ground mode << 2 | fine bays << 4, w palette | which way the face looks << 3 (TF_*)
 *   piers, beams and copings:
 *     uv     as a facade's: metres along the face, the faces' widths (facade_mat)
 *     aux    as a facade's, with the plate's depth (half metres) for the previous face's width
 *     nrm.w  how far out of the glass, in RELIEF_Z_UNITs (interpolated: the reveal's depth)
 *   mullions: uv the vertex's offset from the mullion's foot, across and out (m), which city.vert
 *     shrinks to nothing far off; aux the face's direction along u (0..65535 for -pi..pi)
 * ========================================================================= */

#define RELIEF_Z_UNIT 0.6f
/* which way a trim face looks, in the face's frame */
enum { TF_OUT = 0, TF_LEFT, TF_RIGHT, TF_UP, TF_DOWN };

/* Each building's proportions; the bay's width sets how broad a pier can be. */
typedef struct ReliefStyle { float pierD, shoulderD, beamD, mullD, pierHW, shoulderHW, cornerW, mullHW; int every, glazed; } ReliefStyle;

static ReliefStyle relief_style(const BGraph *g, float bayW) {
    ReliefStyle r;
    uint32_t sh = style_hash(g->seed);
    float h = hash_float(hash_combine(sh, 70u));
    /* one building in six has its bays glazed in several lights, mullions between them (reliefLights in facade.glsl) */
    r.glazed = hash_float(hash_combine(sh, 76u)) < 1.0f / 6.0f;
    r.pierD = 0.36f + 0.14f * h;
    r.shoulderD = r.pierD * 0.62f;
    r.beamD = r.pierD * 0.52f;
    r.mullD = 0.13f;
    r.every = bayW < 3.0f ? 2 : 1;                                 /* narrow bays: a pier on every second line */
    r.pierHW = clampf(bayW * (float)r.every * 0.07f, 0.17f, 0.36f);
    r.shoulderHW = r.pierHW * 0.6f;
    r.cornerW = r.pierHW * 2.2f;
    r.mullHW = 0.035f;
    return r;
}

int relief_on(const BGraph *g, const BSection *s) {
    return open_plan_section(g, s) && s->win == WIN_CURTAIN;
}

float relief_out(const BGraph *g) {
    for (int si = 0; si < g->nsec; ++si)
        if (relief_on(g, &g->sec[si])) return relief_style(g, 5.0f).pierD + 0.02f;
    return 0.0f;
}

static int8_t z_unit(float z) { return (int8_t)(clampf(z / RELIEF_Z_UNIT, 0.0f, 1.0f) * 127.0f + 0.5f); }

/* Quad a,b,c,d (perimeter order) in the current face's frame, its front towards n, uv and depth per corner. */
static void trim_quad_uv(Mesh *m, const vec3 p[4], vec3 n, const float uv[4][2], const int8_t w[4], Mat mt) {
    int idx[4] = { 0, 1, 2, 3 };
    if (v3_dot(v3_cross(v3_sub(p[1], p[0]), v3_sub(p[2], p[0])), n) < 0) { idx[1] = 3; idx[3] = 1; }
    uint8_t facing = n.x < -0.5f ? TF_LEFT : n.x > 0.5f ? TF_RIGHT : n.y > 0.3f ? TF_UP : n.y < -0.3f ? TF_DOWN : TF_OUT;
    uint8_t prm[4] = { mt.p[0], mt.p[1], mt.p[2], (uint8_t)(mt.p[3] | facing << 3) };
    static const int tri[6] = { 0, 1, 2, 0, 2, 3 };
    for (int i = 0; i < 6; ++i) {
        int k = idx[tri[i]];
        mesh_push_w(m, p[k], n, w[k], uv[k][0], uv[k][1], mt.seed, prm, mt.aux);
    }
}

/* A piece of pier, beam or coping: uv as a facade's. */
static void trim_quad(Mesh *m, vec3 a, vec3 b, vec3 c, vec3 d, vec3 n, float pw, Mat mt) {
    vec3 p[4] = { a, b, c, d };
    float uv[4][2];
    int8_t w[4];
    for (int i = 0; i < 4; ++i) { uv[i][0] = p[i].x; uv[i][1] = pw; w[i] = z_unit(p[i].z); }
    trim_quad_uv(m, p, n, uv, w, mt);
}

/* A cross-section (u, z), left to right seen from outside, run from y0 up to y1. */
static void extrude_up(Mesh *m, const float (*s)[2], int n, float y0, float y1, float pw, Mat mt) {
    for (int i = 0; i + 1 < n; ++i) {
        float du = s[i + 1][0] - s[i][0], dz = s[i + 1][1] - s[i][1], len = sqrtf(du * du + dz * dz);
        if (len < 1e-4f) continue;
        trim_quad(m, v3(s[i][0], y0, s[i][1]), v3(s[i + 1][0], y0, s[i + 1][1]), v3(s[i + 1][0], y1, s[i + 1][1]), v3(s[i][0], y1, s[i][1]),
                  v3(-dz / len, 0, du / len), pw, mt);
    }
}

/* A cross-section (z, y), from the top down, run along the face from a to c. Its ends lean by
 * ta and tc per metre out, to meet the neighbouring faces' on the corners' mitres. */
static void extrude_along(Mesh *m, const float (*s)[2], int n, float a, float c, float ta, float tc, float pw, Mat mt) {
    for (int i = 0; i + 1 < n; ++i) {
        float z0 = s[i][0], y0 = s[i][1], z1 = s[i + 1][0], y1 = s[i + 1][1];
        float dz = z1 - z0, dy = y1 - y0, len = sqrtf(dz * dz + dy * dy);
        if (len < 1e-4f) continue;
        trim_quad(m, v3(a - ta * z0, y0, z0), v3(c + tc * z0, y0, z0), v3(c + tc * z1, y1, z1), v3(a - ta * z1, y1, z1),
                  v3(0, dz / len, -dy / len), pw, mt);
    }
}

/* tan(half the turn) at the corner between edges ep and e: how far an offset line runs past the
 * corner per metre out. Positive at a convex corner, negative at a concave one. */
static float corner_lean(const EdgeInfo *ep, const EdgeInfo *e) {
    float cross = ep->ex * e->ez - ep->ez * e->ex, dot = ep->ex * e->ex + ep->ez * e->ez;
    return tanf(0.5f * atan2f(-cross, dot));
}

/* A pier from the street, not just from the top of the ground floor? Over a shopfront's
 * boards and shutters, or in an entrance, it stops above. */
static int pier_lands(const BGraph *g, const BSection *s, int si, int face, float u0, float u1) {
    if (!bsec_grounded(s) || g->groundMode == GF_RETAIL) return 0;
    for (int d = 0; d < g->ndoor; ++d) {
        const BDoor *dr = &g->door[d];
        if (dr->section == si && dr->face == face && u1 > dr->u - dr->width * 0.5f - 0.4f && u0 < dr->u + dr->width * 0.5f + 0.4f) return 0;
    }
    return 1;
}

static int bay_count(const BGraph *g, float len) {
    return (int)maxf(1.0f, floorf(len / bgen_bay_width(g->seed, g->fine) + 0.5f));
}

int relief_ground_piers(const BGraph *g, int si, int k, float (*span)[2], float *depth, int max) {
    const BSection *s = &g->sec[si];
    if (!relief_on(g, s) || k >= s->foot.outer.n) return 0;
    const BLoop *l = &s->foot.outer;
    FaceKey fk = face_key(l, k, 0);
    int nB = bay_count(g, fk.len), n = 0;
    float bayW = fk.len / (float)nB;
    ReliefStyle r = relief_style(g, bayW);
    *depth = r.pierD;
    float w = minf(r.cornerW, fk.len * 0.5f);
    if (fk.startConvex && pier_lands(g, s, si, k, 0.0f, w) && n < max) { span[n][0] = 0.0f; span[n][1] = w; n++; }
    for (int j = r.every; j < nB && n < max; j += r.every) {
        float u = (float)j * bayW;
        if (pier_lands(g, s, si, k, u - r.pierHW, u + r.pierHW)) { span[n][0] = u - r.pierHW; span[n][1] = u + r.pierHW; n++; }
    }
    if (fk.endConvex && pier_lands(g, s, si, k, fk.len - w, fk.len) && n < max) { span[n][0] = fk.len - w; span[n][1] = fk.len; n++; }
    return n;
}

/* The back of a pier, seen across the building through its real windows (impostorSeesOut). */
static void pier_back(Mesh *m, float u0, float u1, float y0, float y1, float pw, Mat mt) {
    trim_quad(m, v3(u0, y0, 0), v3(u1, y0, 0), v3(u1, y1, 0), v3(u0, y1, 0), v3(0, 0, -1), pw, mt);
}

/* The soffit under a pier that doesn't reach the street. */
static void pier_foot(Mesh *m, const ReliefStyle *r, float u0, float u1, float y, float pw, Mat mt) {
    trim_quad(m, v3(u0, y, 0), v3(u1, y, 0), v3(u1, y, r->shoulderD), v3(u0, y, r->shoulderD), v3(0, -1, 0), pw, mt);
    float c = 0.5f * (u0 + u1), hs = 0.5f * (u1 - u0) * 0.6f;
    trim_quad(m, v3(c - hs, y, r->shoulderD), v3(c + hs, y, r->shoulderD), v3(c + hs, y, r->pierD), v3(c - hs, y, r->pierD), v3(0, -1, 0), pw, mt);
}

/* One mullion from y0 to y1 at u: two sides and a front, which city.vert shrinks away to its foot's line far off. */
static void emit_mullion(Mesh *m, float u, float hw, float d, float y0, float y1, float dirAngle, Mat mt) {
    Mat mm = mt;
    mm.aux = (uint32_t)clampf((dirAngle / (2.0f * PI_F) + 0.5f) * 65535.0f + 0.5f, 0.0f, 65535.0f);
    const float s[4][2] = { { -hw, 0.0f }, { -hw, d }, { hw, d }, { hw, 0.0f } };
    const int8_t w[4] = { 0, 0, 0, 0 };
    for (int i = 0; i < 3; ++i) {
        float du = s[i + 1][0] - s[i][0], dz = s[i + 1][1] - s[i][1], len = sqrtf(du * du + dz * dz);
        vec3 p[4] = { v3(u + s[i][0], y0, s[i][1]), v3(u + s[i + 1][0], y0, s[i + 1][1]), v3(u + s[i + 1][0], y1, s[i + 1][1]), v3(u + s[i][0], y1, s[i][1]) };
        float uv[4][2] = { { s[i][0], s[i][1] }, { s[i + 1][0], s[i + 1][1] }, { s[i + 1][0], s[i + 1][1] }, { s[i][0], s[i][1] } };
        trim_quad_uv(m, p, v3(-dz / len, 0, du / len), uv, w, mm);
    }
}

static void emit_face_relief(Mesh *m, const BGraph *g, int si, const BLoop *l, int li, int k, int base) {
    const BSection *s = &g->sec[si];
    FaceKey fk = face_key(l, k, base);
    int n = l->n;
    EdgeInfo ep = edge_of(l, (k + n - 1) % n), e = edge_of(l, k), en = edge_of(l, (k + 1) % n);
    float ta = corner_lean(&ep, &e), tc = corner_lean(&e, &en);
    float len = e.len, top = s->y1, floorH = g->floorH;
    int nB = bay_count(g, len);
    float bayW = len / (float)nB;
    ReliefStyle r = relief_style(g, bayW);
    float firstFloor = G0 + g->groundH;
    int plate = (int)clampf(bfoot_depth_across(&s->foot, l, k) * 2.0f + 0.5f, 0.0f, 127.0f);
    uint32_t aux = (uint32_t)plate | (uint32_t)fk.face << 12 | (uint32_t)fk.next << 18 |
                   (uint32_t)(fk.startConvex | fk.endConvex << 1 | fk.start90 << 2 | fk.end90 << 3) << 24 |
                   (uint32_t)(s->plant & 15) << 28;
    float pw = (float)(((uint32_t)clampf(fk.len * 16.0f + 0.5f, 0.0f, 4095.0f)) | ((uint32_t)clampf(fk.nextLen * 16.0f + 0.5f, 0.0f, 4095.0f)) << 12);
    uint8_t gm = (uint8_t)(g->groundMode << 2 | (g->fine ? 16 : 0));
    Mat pier = mat(MAT_TRIM, g->seed, (uint8_t)si, TRIM_PIER | gm, s->palette), beam = mat(MAT_TRIM, g->seed, (uint8_t)si, TRIM_BEAM | gm, s->palette);
    Mat coping = mat(MAT_TRIM, g->seed, (uint8_t)si, TRIM_COPING | gm, s->palette), mull = mat(MAT_TRIM, g->seed, (uint8_t)si, TRIM_MULLION | gm, s->palette);
    pier.aux = beam.aux = coping.aux = aux;
    Xf sv = xf_save(m);
    enter_face(m, l, k);

    /* where a pier that stops above the ground floor starts: behind the fascia */
    float raised = bsec_grounded(s) ? firstFloor - 0.55f : s->y0;

    /* corner piers, mitred to the neighbouring face's */
    float w = minf(r.cornerW, len * 0.5f), ws = w * 0.6f;
    if (fk.startConvex) {
        const float c[5][2] = { { -ta * r.pierD, r.pierD }, { ws, r.pierD }, { ws, r.shoulderD }, { w, r.shoulderD }, { w, 0.0f } };
        int lands = li == 0 && pier_lands(g, s, si, k, 0.0f, w);
        float y0 = lands ? s->y0 : raised;
        if (top > y0 + 0.1f) {
            extrude_up(m, c, 5, y0, top, pw, pier);
            pier_back(m, 0.0f, w, y0, top, pw, pier);
            if (!lands) pier_foot(m, &r, 0.0f, w, y0, pw, pier);
        }
    }
    if (fk.endConvex) {
        const float c[5][2] = { { len - w, 0.0f }, { len - w, r.shoulderD }, { len - ws, r.shoulderD }, { len - ws, r.pierD }, { len + tc * r.pierD, r.pierD } };
        int lands = li == 0 && pier_lands(g, s, si, k, len - w, len);
        float y0 = lands ? s->y0 : raised;
        if (top > y0 + 0.1f) {
            extrude_up(m, c, 5, y0, top, pw, pier);
            pier_back(m, len - w, len, y0, top, pw, pier);
            if (!lands) pier_foot(m, &r, len - w, len, y0, pw, pier);
        }
    }

    /* piers on the bay lines; mullions between them (none shorter than a floor) */
    float mullFrom = maxf(s->y0, firstFloor);
    float dirAngle = atan2f(m->xs, m->xc);
    int lights = !r.glazed ? 1 : bayW >= 3.6f ? 3 : bayW >= 2.4f ? 2 : 1;   /* as shadeFacade paints them (reliefLights) */
    for (int j = 1; j < nB; ++j) {
        float u = (float)j * bayW;
        if (j % r.every == 0) {
            int lands = li == 0 && pier_lands(g, s, si, k, u - r.pierHW, u + r.pierHW);
            float y0 = lands ? s->y0 : raised;
            if (top > y0 + 0.1f) {
                float hw = r.pierHW, hs = r.shoulderHW, sd = r.shoulderD, d = r.pierD;
                const float c[8][2] = { { u - hw, 0.0f }, { u - hw, sd }, { u - hs, sd }, { u - hs, d }, { u + hs, d }, { u + hs, sd }, { u + hw, sd }, { u + hw, 0.0f } };
                extrude_up(m, c, 8, y0, top, pw, pier);
                pier_back(m, u - hw, u + hw, y0, top, pw, pier);
                if (!lands) pier_foot(m, &r, u - hw, u + hw, y0, pw, pier);
            }
        } else if (top > mullFrom + 1.0f) {
            emit_mullion(m, u, r.mullHW * 1.8f, r.mullD * 1.2f, mullFrom, top, dirAngle, mull);
        }
    }
    if (lights > 1 && top > mullFrom + 1.0f)
        for (int b = 0; b < nB; ++b)
            for (int q = 1; q < lights; ++q)
                emit_mullion(m, ((float)b + (float)q / (float)lights) * bayW, r.mullHW, r.mullD, mullFrom, top, dirAngle, mull);

    /* The beam over each spandrel, behind the piers: a weathered sill sloping out from the glass above, a
     * fascia, and a soffit raked back to the head of the window below (the window's sill and head as in
     * shadeFacade). Three faces, because there is one of these per floor per face: most of a tall building's
     * relief. The first floor's stands out further, and carries down over the ground floor's top. */
    int openTop = roof_free(g, si, e.ax + e.ex * len * 0.5f + e.ez * 0.6f, e.az + e.ez * len * 0.5f - e.ex * 0.6f, 0.0f);
    for (int f = (int)maxf(0.0f, ceilf((s->y0 - firstFloor - 0.1f) / floorH)); f < 400; ++f) {
        float y = firstFloor + (float)f * floorH;
        if (y > top + 0.1f) break;
        if (openTop && y > top - 0.4f) break;   /* the coping's */
        int fascia = f == 0;
        float d = fascia ? r.pierD * 0.8f : r.beamD;
        float b1 = y + 0.0588f * floorH + 0.03f, b0 = y - (fascia ? 0.55f : 0.0812f * floorH + 0.03f);
        b0 = maxf(b0, s->y0); b1 = minf(b1, top);
        float h = b1 - b0;
        if (h < 0.05f) continue;
        const float c[4][2] = { { 0.0f, b1 }, { d, b1 - 0.24f * h }, { d, b1 - 0.68f * h }, { 0.0f, b0 } };
        extrude_along(m, c, 4, 0.0f, len, ta, tc, pw, beam);
    }

    /* the coping along an open roof edge, over the piers' heads */
    if (openTop) {
        float d = r.pierD + 0.05f, b1 = top + 0.14f, b0 = top - 0.34f, h = b1 - b0;
        const float c[4][2] = { { 0.0f, b1 }, { d, b1 }, { d, b1 - 0.6f * h }, { 0.0f, b0 } };
        extrude_along(m, c, 4, 0.0f, len, ta, tc, pw, coping);
    }
    xf_restore(m, sv);
}

void emit_relief(Mesh *m, const BGraph *g, int si) {
    const BSection *s = &g->sec[si];
    if (!relief_on(g, s)) return;
    const BLoop *loops[2] = { &s->foot.outer, &s->foot.hole };
    int base = 0;
    for (int li = 0; li < 2; ++li) {
        for (int k = 0; k < loops[li]->n; ++k) emit_face_relief(m, g, si, loops[li], li, k, base);
        base += loops[li]->n;
    }
}
