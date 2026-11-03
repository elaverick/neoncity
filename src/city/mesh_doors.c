#include "city_internal.h"

/* ===========================================================================
 * Lobbies' automatic doors (interior_auto_doors in src/gen/interior_doors.c),
 * realised in the near layer with the rest of the lobby.
 *
 * What moves is drawn where it stands shut (a revolving door's wings at rest)
 * and moved by shaders/city.vert, so a door costs nothing to animate: its
 * vertices are IS_LEAF (glass) or IS_LEAFFRAME (the rest), params z bit 0
 * says whether they slide (0) or turn (1) and the bits above it the finish
 * (DF_*), aux holds the door's key (FrameUBO.doorKey: how far open, or at
 * what angle), and uv how far the vertex slides when its door is open, or its
 * offset from the drum's axis, both chunk-local. Nothing that moves casts a
 * shadow (city_casts_shadow): the ray-traced shadows' geometry stands still.
 * ========================================================================= */

/* IS_LEAFFRAME finishes (params z >> 1; shadeInterior). */
enum { DF_DARK = 0, DF_BRONZE, DF_STEEL, DF_FROST, DF_RUBBER };

static Mat dmat(const BGraph *g, const ILayout *L, int surface, int sub) {
    return mat(MAT_INTERIOR, g->seed, (uint8_t)surface, (uint8_t)sub, q_half(L->ceilY - L->floorY));
}

/* Enters door d's frame at (px, pz) in the building's: x along a, z along
 * sg * n (returns sg, so the door's q along n is z * sg). */
static float enter_door(Mesh *m, const IAutoDoor *d, float px, float pz) {
    xf_enter(m, px, pz, d->ax, d->az);
    return -d->az * d->nx + d->ax * d->nz > 0.0f ? 1.0f : -1.0f;
}

/* A box in the door's frame: s along it, q across it (sg: enter_door's). */
static void dbox(Mesh *m, float s0, float s1, float q0, float q1, float y0, float y1, float sg, int faces, Mat mt) {
    float z0 = sg * q0, z1 = sg * q1;
    emit_box(m, v3(minf(s0, s1), y0, minf(z0, z1)), v3(maxf(s0, s1), y1, maxf(z0, z1)), faces, mt, UV_FACADE);
}

/* The vertices from v0 on slide with door `key`: `travel` along the frame's x when it is open. */
static void slide_since(Mesh *m, uint32_t v0, uint32_t key, float travel) {
    float dx = m->xf ? m->xc * travel : travel, dz = m->xf ? m->xs * travel : 0.0f;
    for (uint32_t i = v0; i < m->count; ++i) { m->v[i].uv[0] = dx; m->v[i].uv[1] = dz; m->v[i].aux = key; }
}

/* ... or turn with it about the frame's origin (the drum's axis). */
static void turn_since(Mesh *m, uint32_t v0, uint32_t key) {
    for (uint32_t i = v0; i < m->count; ++i) { m->v[i].uv[0] = m->v[i].pos[0] - m->xx; m->v[i].uv[1] = m->v[i].pos[2] - m->xz; m->v[i].aux = key; }
}

typedef struct LeafMats { Mat glass, frame, band, sweep; } LeafMats;

static LeafMats leaf_mats(const BGraph *g, const ILayout *L, int finish, int turns) {
    LeafMats lm;
    lm.glass = dmat(g, L, IS_LEAF, turns);
    lm.frame = dmat(g, L, IS_LEAFFRAME, finish << 1 | turns);
    lm.band  = dmat(g, L, IS_LEAFFRAME, DF_FROST << 1 | turns);
    lm.sweep = dmat(g, L, IS_LEAFFRAME, DF_RUBBER << 1 | turns);
    return lm;
}

/* One leaf or wing in its own frame, x along it from x0 to x1, its glass at
 * z = 0: stiles, rails, the pane, and a frosted band at eye height so the
 * glass is seen (y0: the floor). tip: a rubber sweep on the end at x1, a
 * wing's against its drum. */
static void emit_leaf(Mesh *m, float x0, float x1, float y0, float y1, int tip, const LeafMats *lm) {
    const float st = 0.045f, d = 0.022f, b = y0 - 0.03f;
    emit_box(m, v3(x0, y0, -d), v3(x0 + st, y1, d), F_NOBOT | F_BOT, lm->frame, UV_FACADE);
    emit_box(m, v3(x1 - st, y0, -d), v3(x1, y1, d), F_NOBOT | F_BOT, lm->frame, UV_FACADE);
    emit_box(m, v3(x0 + st, y0, -d), v3(x1 - st, y0 + 0.1f, d), F_SIDES | F_TOP | F_BOT, lm->frame, UV_FACADE);
    emit_box(m, v3(x0 + st, y1 - 0.08f, -d), v3(x1 - st, y1, d), F_SIDES | F_TOP | F_BOT, lm->frame, UV_FACADE);
    emit_box(m, v3(x0 + st, y0 + 0.1f, -0.006f), v3(x1 - st, y1 - 0.08f, 0.006f), F_NZ | F_PZ, lm->glass, UV_FACADE);
    emit_box(m, v3(x0 + st, b + 1.42f, -0.007f), v3(x1 - st, b + 1.5f, 0.007f), F_NZ | F_PZ, lm->band, UV_FACADE);
    if (tip) emit_box(m, v3(x1, y0 + 0.02f, -0.008f), v3(x1 + 0.03f, y1 - 0.02f, 0.008f), F_NOBOT | F_BOT, lm->sweep, UV_FACADE);
}

/* A sliding door: its leaves on their tracks, the operator's header over
 * them the width of their travel, a guide plate on the floor, and the
 * sensor under the header on the far side from the leaves. */
static void emit_sliding(Mesh *m, const BGraph *g, const ILayout *L, const IAutoDoor *d, int finish) {
    Xf sv = xf_save(m);
    float sg = enter_door(m, d, d->cx, d->cz);
    float y0 = d->y0, h = d->height, reach = interior_door_reach(IE_SLIDE, d->leaves, d->width);
    float t0 = 1e9f, t1 = -1e9f;
    LeafMats lm = leaf_mats(g, L, finish, 0);
    for (int k = 0; k < d->leaves; ++k) {
        float s0, s1, tr, o0, o1, otr;
        interior_leaf(d->leaves, d->outer, d->width, k, 0.0f, &s0, &s1, &tr);
        interior_leaf(d->leaves, d->outer, d->width, k, 1.0f, &o0, &o1, &otr);
        t0 = minf(t0, tr); t1 = maxf(t1, tr);
        Xf lv = xf_save(m);
        xf_enter(m, 0.0f, sg * tr, 1.0f, 0.0f);
        uint32_t v0 = m->count;
        emit_leaf(m, s0, s1, y0 + 0.03f, y0 + h - 0.01f, 0, &lm);
        slide_since(m, v0, d->key, o0 - s0);
        xf_restore(m, lv);
    }
    Mat metal = dmat(g, L, IS_METAL, finish), dark = dmat(g, L, IS_METAL, 0);
    float q0 = minf(t0, t1) - 0.06f, q1 = maxf(t0, t1) + 0.06f;
    if (d->outer) q1 += 0.05f; else q0 -= 0.05f;   /* the operator's box stands out from the leaves, away from the glass */
    dbox(m, -reach, reach, q0, q1, y0 + h, y0 + h + IE_HEADER, sg, F_NOBOT | F_BOT, metal);
    dbox(m, -reach + 0.1f, reach - 0.1f, minf(t0, t1) - 0.03f, maxf(t0, t1) + 0.03f, y0, y0 + 0.025f, sg, F_NOBOT, dark);
    float qs = d->outer ? q1 - 0.05f : q0 + 0.05f;
    dbox(m, -0.12f, 0.12f, qs - 0.04f, qs + 0.04f, y0 + h - 0.06f, y0 + h, sg, F_SIDES | F_BOT, dark);
    dbox(m, 0.07f, 0.09f, qs - 0.01f, qs + 0.01f, y0 + h - 0.065f, y0 + h - 0.06f, sg, F_BOT, dmat(g, L, IS_SIGN, 0));
    xf_restore(m, sv);
}

/* A cylindrical band from angle p0 to p1 (from -n towards a; sg: the door
 * frame's) between radii r0 and r1, y0 to y1: its inner and outer faces. */
static void emit_arc(Mesh *m, float r0, float r1, float p0, float p1, int nseg, float y0, float y1, float sg, Mat mt) {
    for (int i = 0; i < nseg; ++i) {
        float a0 = p0 + (p1 - p0) * (float)i / (float)nseg, a1 = p0 + (p1 - p0) * (float)(i + 1) / (float)nseg, am = 0.5f * (a0 + a1);
        float c0x = sinf(a0), c0z = -sg * cosf(a0), c1x = sinf(a1), c1z = -sg * cosf(a1);
        vec3 nrm = v3(sinf(am), 0.0f, -sg * cosf(am));
        float uv[4][2] = { { r1 * a0, y0 }, { r1 * a1, y0 }, { r1 * a1, y1 }, { r1 * a0, y1 } };
        emit_quad(m, v3(c0x * r1, y0, c0z * r1), v3(c1x * r1, y0, c1z * r1), v3(c1x * r1, y1, c1z * r1), v3(c0x * r1, y1, c0z * r1), nrm, uv, mt);
        emit_quad(m, v3(c0x * r0, y0, c0z * r0), v3(c1x * r0, y0, c1z * r0), v3(c1x * r0, y1, c1z * r0), v3(c0x * r0, y1, c0z * r0), v3_scale(nrm, -1.0f), uv, mt);
    }
}

/* A disc's sector from p0 to p1 between radii r0 and r1 at height y, facing up or down. */
static void emit_ring(Mesh *m, float r0, float r1, float p0, float p1, int nseg, float y, int down, float sg, Mat mt) {
    for (int i = 0; i < nseg; ++i) {
        float a0 = p0 + (p1 - p0) * (float)i / (float)nseg, a1 = p0 + (p1 - p0) * (float)(i + 1) / (float)nseg;
        float c0x = sinf(a0), c0z = -sg * cosf(a0), c1x = sinf(a1), c1z = -sg * cosf(a1);
        float uv[4][2] = { { c0x * r0, c0z * r0 }, { c1x * r0, c1z * r0 }, { c1x * r1, c1z * r1 }, { c0x * r1, c0z * r1 } };
        emit_quad(m, v3(c0x * r0, y, c0z * r0), v3(c1x * r0, y, c1z * r0), v3(c1x * r1, y, c1z * r1), v3(c0x * r1, y, c0z * r1),
                  v3(0.0f, down ? -1.0f : 1.0f, 0.0f), uv, mt);
    }
}

/* A revolving door: the drum's curved glass either side, glass flanking the
 * way in from the facade to the drum's middle, the canopy over both with a
 * ring of light under it, and the wings on their post, which turn. */
static void emit_revolving(Mesh *m, const BGraph *g, const ILayout *L, const IAutoDoor *d, int finish) {
    Xf sv = xf_save(m);
    float sg = enter_door(m, d, d->cx, d->cz);
    float R = d->radius, y0 = d->y0, h = d->height, yt = y0 + h, front = R + IE_DRUM_GAP;
    Mat glass = dmat(g, L, IS_GLASS, 0), metal = dmat(g, L, IS_METAL, finish), dark = dmat(g, L, IS_METAL, 0);
    /* the drum's walls: arcs centred on either side, each as wide as a compartment, so a wing always seals it */
    float span = PI_F / (float)d->leaves;
    for (int side = -1; side <= 1; side += 2) {
        float pc = (float)side * PI_F * 0.5f, p0 = pc - span, p1 = pc + span;
        emit_arc(m, R + 0.01f, R + 0.022f, p0, p1, 12, y0 + 0.08f, yt - 0.08f, sg, glass);
        emit_arc(m, R + 0.0f, R + 0.035f, p0, p1, 12, y0, y0 + 0.08f, sg, metal);
        emit_arc(m, R + 0.0f, R + 0.035f, p0, p1, 12, yt - 0.08f, yt, sg, metal);
        for (int e = 0; e < 2; ++e) {   /* posts at the arc's ends */
            float p = e ? p1 : p0, px = sinf(p) * (R + 0.02f), pz = -sg * cosf(p) * (R + 0.02f);
            emit_box(m, v3(px - 0.03f, y0, pz - 0.03f), v3(px + 0.03f, yt, pz + 0.03f), F_SIDES, metal, UV_FACADE);
        }
        /* the flanking glass, from the facade's jamb to the drum's middle */
        float s = (float)side * (R + 0.03f);
        dbox(m, s - 0.006f, s + 0.006f, -front + 0.04f, 0.0f, y0 + 0.08f, yt - 0.08f, sg, F_NX | F_PX, glass);
        dbox(m, s - 0.025f, s + 0.025f, -front, 0.0f, y0, y0 + 0.08f, sg, F_NOBOT, metal);
        dbox(m, s - 0.025f, s + 0.025f, -front, 0.0f, yt - 0.08f, yt, sg, F_SIDES | F_BOT, metal);
        dbox(m, s - 0.03f, s + 0.03f, -front, -front + 0.06f, y0, yt, sg, F_SIDES, metal);
    }
    /* the canopy: a box over the way in, a half drum's over the rest, a ring of light under it */
    float cr = R + 0.06f, ch = 0.25f;
    dbox(m, -cr, cr, -front, 0.0f, yt, yt + ch, sg, F_NOBOT | F_BOT, metal);
    emit_arc(m, cr - 0.02f, cr, PI_F * 0.5f, PI_F * 1.5f, 24, yt, yt + ch, sg, metal);
    emit_ring(m, 0.0f, cr, PI_F * 0.5f, PI_F * 1.5f, 24, yt + ch, 0, sg, metal);
    emit_ring(m, 0.0f, cr, PI_F * 0.5f, PI_F * 1.5f, 24, yt + 0.001f, 1, sg, dmat(g, L, IS_CEILING, 0));
    emit_ring(m, R - 0.24f, R - 0.2f, 0.0f, 2.0f * PI_F, 48, yt - 0.002f, 1, sg, dmat(g, L, IS_LIGHT, 4));
    emit_ring(m, R - 0.02f, R + 0.04f, 0.0f, 2.0f * PI_F, 48, y0 + 0.022f, 0, sg, dark);   /* the floor's edge ring */
    /* the post and the wings, which turn */
    LeafMats lm = leaf_mats(g, L, finish, 1);
    uint32_t v0 = m->count;
    emit_box(m, v3(-0.06f, y0, -0.06f), v3(0.06f, yt, 0.06f), F_SIDES, lm.frame, UV_FACADE);
    for (int k = 0; k < d->leaves; ++k) {
        float p = interior_wing_angle(d->leaves, k);
        Xf wv = xf_save(m);
        xf_enter(m, 0.0f, 0.0f, sinf(p), -sg * cosf(p));
        emit_leaf(m, 0.06f, R - 0.035f, y0 + 0.03f, yt - 0.02f, 1, &lm);
        xf_restore(m, wv);
    }
    turn_since(m, v0, d->key);
    xf_restore(m, sv);
}

void emit_auto_doors(Mesh *m, const BGraph *g, const ILayout *L) {
    IAutoDoor dd[IE_MAX_DOORS];
    int n = interior_auto_doors(g, L, dd, IE_MAX_DOORS);
    float pick = hash_float(hash_combine(style_hash(g->seed), 0xD00F1u));
    int finish = pick < 0.55f ? DF_DARK : pick < 0.85f ? DF_STEEL : DF_BRONZE;
    for (int k = 0; k < n; ++k) {
        if (dd[k].type == IE_REVOLVE) emit_revolving(m, g, L, &dd[k], finish);
        else emit_sliding(m, g, L, &dd[k], finish);
    }
}
