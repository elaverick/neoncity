#include "kit.h"
#include "floor_gen.h"
#include "furniture.h"
#include "plant.h"

/* A planned floor (floor_gen.c) as a scene: each item built from the furniture (furniture.h) and the
 * kit's boxes and meshes, its lights on as the module's light state says. What has no model of its
 * own (books, printers, tables, sofas' neighbours) stays boxes. */

/* What a generated floor adds to the kit. */
typedef struct Extra {
    int pedLight, mug, book[4], canvas, ink[5], panel, leather, rugs[3];
    int cyl;      /* a cylinder of radius 1, height 1, standing on y = 0 */
} Extra;

static int cylinder_mesh(Scene *s) {
    enum { N = 14 };
    float v[(N * 2 + 2) * 3];
    uint32_t ix[N * 12], n = 0;
    for (int i = 0; i < N; ++i) {
        float a = (float)i / N * 2.0f * PI_F;
        v[i * 6 + 0] = cosf(a); v[i * 6 + 1] = 0.0f; v[i * 6 + 2] = sinf(a);
        v[i * 6 + 3] = cosf(a); v[i * 6 + 4] = 1.0f; v[i * 6 + 5] = sinf(a);
    }
    v[N * 6 + 0] = 0; v[N * 6 + 1] = 0; v[N * 6 + 2] = 0;
    v[N * 6 + 3] = 0; v[N * 6 + 4] = 1; v[N * 6 + 5] = 0;
    for (uint32_t i = 0; i < N; ++i) {
        uint32_t j = (i + 1) % N, a = i * 2, b = a + 1, c = j * 2, d = c + 1, bot = N * 2, top = N * 2 + 1;
        uint32_t t[12] = { a, b, c, c, b, d, top, d, b, bot, a, c };
        memcpy(ix + n, t, sizeof(t));
        n += 12;
    }
    return mesh_add(s, v, N * 2 + 2, ix, n);
}

static Extra extra_make(Scene *s, Kit *k, int style) {
    Extra e;
    e.pedLight = mat_add(s, 0.62f, 0.60f, 0.56f, 0, 0, 0);      /* the reference's pale pedestals */
    mat_surface(s, e.pedLight, 0.4f, 0.0f, 0.0f, 0.0f);
    e.mug = mat_add(s, 0.05f, 0.05f, 0.055f, 0, 0, 0);
    mat_surface(s, e.mug, 0.25f, 0.0f, 0.0f, 0.0f);
    static const float B[4][3] = { { 0.28f, 0.10f, 0.07f }, { 0.08f, 0.13f, 0.22f }, { 0.45f, 0.40f, 0.30f }, { 0.10f, 0.16f, 0.10f } };
    for (int i = 0; i < 4; ++i) e.book[i] = mat_add(s, B[i][0], B[i][1], B[i][2], 0, 0, 0);
    e.canvas = mat_add(s, 0.62f, 0.58f, 0.50f, 0, 0, 0);
    static const float I[5][3] = { { 0.03f, 0.03f, 0.035f }, { 0.55f, 0.14f, 0.06f }, { 0.65f, 0.38f, 0.10f }, { 0.08f, 0.16f, 0.25f }, { 0.20f, 0.05f, 0.16f } };
    for (int i = 0; i < 5; ++i) e.ink[i] = mat_add(s, I[i][0], I[i][1], I[i][2], 0, 0, 0);
    e.panel = mat_add(s, 0.09f, 0.06f, 0.04f, 0, 0, 0);          /* a wood-panelled wall */
    mat_surface(s, e.panel, 0.4f, 0.0f, 0.0f, 0.0f);
    e.leather = mat_add(s, 0.05f, 0.03f, 0.022f, 0, 0, 0);
    mat_surface(s, e.leather, 0.45f, 0.0f, 0.0f, 0.0f);
    static const float R[3][3] = { { 0.16f, 0.09f, 0.06f }, { 0.07f, 0.09f, 0.12f }, { 0.13f, 0.12f, 0.10f } };
    for (int i = 0; i < 3; ++i) { e.rugs[i] = mat_add(s, R[i][0], R[i][1], R[i][2], 0, 0, 0); mat_surface(s, e.rugs[i], 1.0f, 0.0f, 0.0f, 0.0f); }
    e.cyl = cylinder_mesh(s);
    /* the styles' surfaces */
    if (style == FS_EXEC) {
        BakeMat *w = &s->mats[k->wood];
        w->albedo[0] = 0.14f; w->albedo[1] = 0.075f; w->albedo[2] = 0.04f;   /* warm walnut */
    }
    if (style == FS_DENSE) {   /* contract carpet: the floor that doesn't mirror */
        BakeMat *f = &s->mats[k->floor];
        f->albedo[0] = 0.075f; f->albedo[1] = 0.08f; f->albedo[2] = 0.09f;
        mat_surface(s, k->floor, 0.95f, 0.0f, 0.0f, 0.0f);
    }
    return e;
}

static float yaw_of(int dir) { return (float)dir * PI_F * 0.5f; }

/* A float in 0..1 from a seed and a tag, for how a thing looks: never the layout. */
static float look(uint32_t seed, uint32_t tag) {
    uint32_t s = (seed ^ (tag * 0x9E3779B1u)) | 1u;
    for (int i = 0; i < 3; ++i) { s ^= s << 13; s ^= s >> 17; s ^= s << 5; }
    return (float)(s >> 8) * (1.0f / 16777216.0f);
}

/* A variant for one piece: mostly the one its floor's run of them shares, the odd one out of step. */
static int variant_of(uint32_t seed, uint32_t tag) {
    return look(seed, tag) < 0.7f ? (int)(look(seed & 0xFFFFFF00u, tag + 1u) * 2.99f) : (int)(look(seed, tag + 2u) * 2.99f);
}

/* A thin card of a material, centred at (x, y, z) in the frame turned by yaw; its image on the face to look at. */
static void card_at(Scene *s, const Furniture *f, int mat, float x, float y, float z, float yaw, float w, float h, float d) {
    inst_add(s, f->card, mat, 0, x, y, z, w, h, d, yaw);
}

static void build_desk(Scene *s, const Kit *k, const Extra *e, const Furniture *f, const FloorItem *it, int state) {
    float yaw = yaw_of(it->dir), w = it->a, ox = it->ox, oz = it->oz, c = cosf(yaw), n = sinf(yaw);
    uint32_t sd = it->seed;
#define WX(LX, LZ) (ox + c * (LX) + n * (LZ))
#define WZ(LX, LZ) (oz - n * (LX) + c * (LZ))
#define B(M, FL, X, Y, Z, SX, SY, SZ) box_local(s, (M), (FL), ox, oz, yaw, (X), (Y), (Z), (SX), (SY), (SZ), 0.0f)
    /* The desk, stretched to its plan: back 5 cm clear of the line it stands on, where a partition's feet go. */
    const float back = 0.05f, depth = 0.75f, top = f->desk.mx[1];
    float dw = f->desk.mx[0] - f->desk.mn[0], dd = f->desk.mx[2] - f->desk.mn[2];
    piece_place(s, &f->desk, variant_of(sd, 70u), BF_LEAKY, WX(0.0f, back + depth * 0.5f), 0.0f, WZ(0.0f, back + depth * 0.5f), yaw, w / dw, 1.0f, depth / dd);
    if ((it->bit[3] >> state) & 1u) B(k->stripOn, BF_EMITTER, 0.0f, top - 0.056f, 0.3f, 0.6f, 0.004f, 0.015f);   /* in the kneehole, under the top */

    /* the computers, facing their user */
    const Piece *pc = &f->computer;
    float cz = back + 0.02f + (pc->mx[2] - pc->mn[2]) * 0.5f;
    for (int i = 0; i < it->variant; ++i) {
        float cx = it->variant == 1 ? 0.0f : (i == 0 ? -0.28f : 0.28f);
        piece_place(s, pc, 0, 0, WX(cx, cz), top, WZ(cx, cz), yaw, 1.0f, 1.0f, 1.0f);
        if ((it->bit[i] >> state) & 1u)
            inst_add(s, pc->extra, f->screen[(int)(look(sd, 80u + (uint32_t)i) * 3.99f)], BF_EMITTER, WX(cx, cz), top, WZ(cx, cz), 1, 1, 1, yaw);
    }

    /* a banker's lamp at the back corner, its shade towards the user */
    float free = it->b != 0.0f ? -it->b : (look(sd, 1u) < 0.5f ? 1.0f : -1.0f);   /* the side without it */
    if (it->b != 0.0f) {
        float lx = it->b * (w * 0.5f - 0.16f), lz = back + 0.17f;
        bool on = (it->bit[2] >> state) & 1u;
        piece_place(s, &f->lamp, 0, 0, WX(lx, lz), top, WZ(lx, lz), yaw, 1.0f, 1.0f, 1.0f);
        if (on) {
            inst_add(s, f->lamp.extra, k->lampOn, BF_EMITTER, WX(lx, lz), top, WZ(lx, lz), 1, 1, 1, yaw);
            light_add(s, WX(lx, lz + 0.05f), top + 0.25f, WZ(lx, lz + 0.05f), 0.9f, 0.52f, 0.22f);
        }
    }

    /* what people leave out */
    float y = top + 0.0015f;
    int sheets = it->variant == 0 ? (look(sd, 5u) < 0.3f ? 1 : 0) : (look(sd, 5u) < 0.7f ? 1 + (int)(look(sd, 6u) * 2.99f) : 0);
    float px = -free * w * (0.1f + 0.22f * look(sd, 7u));
    for (int i = 0; i < sheets; ++i) {   /* A4, on its side across the desk's front */
        float spin = PI_F * 0.5f + (look(sd, 90u + (uint32_t)i) - 0.5f) * 0.4f;
        float sx = px + (look(sd, 95u + (uint32_t)i) - 0.5f) * 0.05f, sz = back + 0.62f + (look(sd, 99u + (uint32_t)i) - 0.5f) * 0.04f;
        card_at(s, f, f->paper[(int)(look(sd, 103u + (uint32_t)i) * 3.99f)], WX(sx, sz), y, WZ(sx, sz), yaw + spin, 0.297f, 0.0012f, 0.21f);
        y += 0.0014f;
    }
    if (sheets && look(sd, 8u) < 0.5f)
        piece_place(s, &f->pen, 0, 0, WX(px, back + 0.62f), y, WZ(px, back + 0.62f), yaw + look(sd, 9u) * 6.28f, 1.0f, 1.0f, 1.0f);
    if (look(sd, 50u) < 0.5f) {
        float x = free * (w * 0.5f - 0.13f), z = back + 0.58f;
        piece_place(s, &f->phone, 0, 0, WX(x, z), top, WZ(x, z), yaw + (look(sd, 51u) - 0.5f) * 0.6f, 1.0f, 1.0f, 1.0f);
    }
    if (it->variant < 2 && w >= 1.5f && look(sd, 52u) < 0.45f) {
        float x = free * (w * 0.5f - 0.2f), z = back + 0.16f;
        piece_place(s, &f->intray, 0, BF_OPEN, WX(x, z), top, WZ(x, z), yaw + (look(sd, 53u) - 0.5f) * 0.2f, 1.0f, 1.0f, 1.0f);
    } else if (it->variant == 1 && look(sd, 3u) < 0.45f) {   /* or a stack of books */
        float bx = free * (w * 0.5f - 0.16f);
        int books = 2 + (int)(look(sd, 4u) * 3.0f);
        for (int i = 0; i < books; ++i)
            box_local(s, e->book[(int)(look(sd, 10u + (uint32_t)i) * 3.99f)], 0, ox, oz, yaw, bx, top + 0.017f + 0.036f * (float)i, back + 0.2f,
                      0.20f + 0.04f * look(sd, 20u + (uint32_t)i), 0.034f, 0.27f, (look(sd, 30u + (uint32_t)i) - 0.5f) * 0.5f);
    }
    if (it->variant > 0 && look(sd, 54u) < 0.6f) {
        float mx = free * w * 0.3f, mz = back + 0.48f;
        inst_add(s, e->cyl, e->mug, 0, WX(mx, mz), top, WZ(mx, mz), 0.04f, 0.095f, 0.04f, 0.0f);
    }

    /* the chair, somewhere near its desk, facing it */
    float jx = (look(sd, 55u) - 0.5f) * 0.3f, sp = (look(sd, 40u) - 0.5f) * 0.7f, lz = 1.22f;
    piece_place(s, &f->chair, variant_of(sd, 60u), BF_LEAKY, WX(jx, lz), 0.0f, WZ(jx, lz), yaw + PI_F + sp, 1.0f, 1.0f, 1.0f);
#undef B
}

/* A run of partitions on their feet between the desks: one per desk's width or so, stretched to the
 * run's share and the plan's height, narrow in the foot so it stands between the desks' backs. A chart
 * or two pinned on the pale upper panels. */
static void build_panel(Scene *s, const Kit *k, const Furniture *f, const FloorItem *it, int state) {
    const Piece *p = &f->partition;
    bool alongZ = it->dir == 1;
    float len = alongZ ? it->z1 - it->z0 : it->x1 - it->x0, yaw = alongZ ? PI_F * 0.5f : 0.0f;
    int n = (int)fmaxf(roundf(len / 1.7f), 1.0f);
    float seg = len / (float)n, pw = p->mx[0] - p->mn[0], ph = p->mx[1] - p->mn[1], pd = p->mx[2] - p->mn[2];
    float sz = 0.09f / pd, face = 0.0177f * 0.84f * sz + 0.0015f;   /* the panel's half thickness, as packed, stretched: and a card's */
    float c = cosf(yaw), m = sinf(yaw);
    for (int i = 0; i < n; ++i) {
        float along = ((float)i + 0.5f) * seg;
        float x = alongZ ? (it->x0 + it->x1) * 0.5f : it->x0 + along, z = alongZ ? it->z0 + along : (it->z0 + it->z1) * 0.5f;
        piece_place(s, p, 0, 0, x, 0.0f, z, yaw, seg / pw, FLOOR_PANEL_H / ph, sz);
        uint32_t sd = (uint32_t)(i * 2654435761u) ^ (uint32_t)(x * 97.0f) ^ ((uint32_t)(z * 89.0f) << 12);
        for (int side = 0; side < 2; ++side) {
            uint32_t t = (uint32_t)side * 16u;
            if (look(sd, 1u + t) > 0.55f) continue;
            float lx = (look(sd, 2u + t) - 0.5f) * (seg - 0.6f), lz = side ? -face : face;
            float wx = x + c * lx + m * lz, wz = z - m * lx + c * lz;
            inst_add(s, f->card, f->chart[(int)(look(sd, 3u + t) * 5.99f)], 0, wx, 1.08f + (look(sd, 4u + t) - 0.5f) * 0.05f, wz, 0.42f, 0.297f, 0.002f, yaw + (side ? PI_F : 0.0f));
        }
    }
    if ((it->bit[0] >> state) & 1u) box(s, k->coveOn, BF_EMITTER, it->x0, FLOOR_PANEL_H, it->z0, it->x1, FLOOR_PANEL_H + 0.004f, it->z1);
}

static void build_pendant(Scene *s, const Kit *k, const FloorItem *it, float y, int state) {
    bool on = (it->bit[0] >> state) & 1u;
    float x = it->ox, z = it->oz, hx = it->dir ? 0.06f : 0.8f, hz = it->dir ? 0.8f : 0.06f, dx = it->dir ? 0.05f : 0.78f, dz = it->dir ? 0.78f : 0.05f;
    box(s, k->pendantOff, BF_EMITTER, x - hx, y, z - hz, x + hx, y + 0.05f, z + hz);
    if (on) {
        box(s, k->pendantOn, BF_EMITTER, x - dx, y - 0.004f, z - dz, x + dx, y, z + dz);
        box(s, k->pendantUp, BF_EMITTER, x - dx, y + 0.05f, z - dz, x + dx, y + 0.054f, z + dz);
        light_add(s, x, y - 0.03f, z, 9.0f, 6.8f, 4.3f);
    }
    float wx = it->dir ? 0.0f : 0.7f, wz = it->dir ? 0.7f : 0.0f;
    for (int w = -1; w <= 1; w += 2)
        box(s, k->wire, 0, x + wx * (float)w - 0.004f, y + 0.05f, z + wz * (float)w - 0.004f, x + wx * (float)w + 0.004f, s->ceilY, z + wz * (float)w + 0.004f);
}

static void build_room(Scene *s, const Kit *k, const Furniture *f, const FloorItem *it, int state) {
    float xa = it->x0, xb = it->x1, za = it->z0, zb = it->z1, g = 0.006f, c = s->ceilY, top = fminf(2.7f, c - 0.2f), door = it->a;
    float zd = it->dir == 0 ? za : zb, zo = it->dir == 0 ? zb : za;
    box(s, k->glass, BF_GLASS, xa, 0.0f, zo - g, xb, top, zo + g);
    box(s, k->glass, BF_GLASS, xa - g, 0.0f, za, xa + g, top, zb);
    box(s, k->glass, BF_GLASS, xb - g, 0.0f, za, xb + g, top, zb);
    box(s, k->glass, BF_GLASS, xa, 0.0f, zd - g, door - 0.5f, top, zd + g);
    box(s, k->glass, BF_GLASS, door + 0.5f, 0.0f, zd - g, xb, top, zd + g);
    box(s, k->glass, BF_GLASS, door - 0.5f, 2.15f, zd - g, door + 0.5f, top, zd + g);
    for (int side = 0; side < 2; ++side) {
        float z = side ? zb : za;
        box(s, k->coreWall, 0, xa - 0.05f, top, z - 0.05f, xb + 0.05f, c, z + 0.05f);
        for (float x = xa; x <= xb + 0.01f; x += (xb - xa) / 4.0f)
            if (side != (it->dir == 2) || fabsf(x - door) > 0.53f) box(s, k->mullion, 0, x - 0.025f, 0.0f, z - 0.03f, x + 0.025f, top, z + 0.03f);
        /* the manifestation band */
        for (int part = 0; part < 2; ++part) {
            float a = part ? door + 0.5f : xa, b = part ? xb : door - 0.5f;
            if (side != (it->dir == 2)) { if (part) continue; a = xa; b = xb; }
            box(s, k->band, 0, a, 1.0f, z - g - 0.003f, b, 1.1f, z - g);
        }
    }
    box(s, k->coreWall, 0, xa - 0.05f, top, za, xa + 0.05f, c, zb);
    box(s, k->coreWall, 0, xb - 0.05f, top, za, xb + 0.05f, c, zb);
    box(s, k->frame, 0, door - 0.53f, 0.0f, zd - 0.03f, door - 0.5f, 2.15f, zd + 0.03f);
    box(s, k->frame, 0, door + 0.5f, 0.0f, zd - 0.03f, door + 0.53f, 2.15f, zd + 0.03f);
    box(s, k->frame, 0, door - 0.53f, 2.15f, zd - 0.03f, door + 0.53f, 2.2f, zd + 0.03f);
    FloorTable t = floor_room_table(it);
    box(s, k->wood, 0, t.x0, 0.72f, t.z0, t.x1, 0.76f, t.z1);
    box(s, k->frame, 0, t.x0 + 0.4f, 0.0f, (t.z0 + t.z1) * 0.5f - 0.1f, t.x1 - 0.4f, 0.72f, (t.z0 + t.z1) * 0.5f + 0.1f);
    uint32_t i = 0;
    for (float x = t.x0 + 0.5f; x < t.x1 - 0.2f; x += 1.0f, ++i) {
        /* a chair's front is its +z: towards the table */
        piece_place(s, &f->chair, variant_of(it->seed, 300u), BF_LEAKY, x, 0.0f, t.z0 - 0.48f, (look(it->seed, 100u + i) - 0.5f) * 0.5f, 1.0f, 1.0f, 1.0f);
        piece_place(s, &f->chair, variant_of(it->seed, 300u), BF_LEAKY, x, 0.0f, t.z1 + 0.48f, PI_F + (look(it->seed, 200u + i) - 0.5f) * 0.5f, 1.0f, 1.0f, 1.0f);
    }
    /* a blackboard at the room's far end from its door, facing the table, where there's room for it and its feet */
    float bw = f->blackboard.mx[0] - f->blackboard.mn[0];
    if (zb - za >= bw + 0.3f && look(it->seed, 400u) < 0.75f) {
        bool atMax = fabsf(door - xa) < fabsf(door - xb);
        float bx = atMax ? xb - 0.4f : xa + 0.4f;
        piece_place(s, &f->blackboard, 0, 0, bx, 0.0f, (za + zb) * 0.5f, atMax ? -PI_F * 0.5f : PI_F * 0.5f, 1.0f, 1.0f, 1.0f);
    }
    if ((it->bit[0] >> state) & 1u) box(s, k->coveOn, BF_EMITTER, xa, top - 0.02f, za + 0.05f, xb, top, za + 0.07f);
}

static void build_lounge(Scene *s, Kit *k, const Extra *e, const FloorItem *it, const char *assets, int state) {
    float x = it->ox, z = it->oz, f = it->dir == 2 ? -1.0f : 1.0f;
#define ZB(A, B) fminf(z + f * (A), z + f * (B)), fmaxf(z + f * (A), z + f * (B))
    float a, b;
    box(s, e->rugs[(int)(look(it->seed, 1u) * 2.99f)], 0, x - 1.4f, 0.0f, z - 1.1f, x + 1.4f, 0.012f, z + 1.1f);
    sofa(s, k, x, z - f * 0.6f, it->dir == 2 ? PI_F : 0.0f, assets);
    a = fminf(z + f * 0.15f, z + f * 0.55f); b = fmaxf(z + f * 0.15f, z + f * 0.55f);
    box(s, k->marble, 0, x - 0.45f, 0.0f, a, x + 0.45f, 0.38f, b);
    a = fminf(z - f * 0.35f, z - f * 0.85f); b = fmaxf(z - f * 0.35f, z - f * 0.85f);
    box(s, k->marble, 0, x + 1.45f, 0.0f, a, x + 1.95f, 0.5f, b);
    box(s, k->marble, 0, x - 1.97f, 0.0f, a, x - 1.47f, 0.5f, b);
    bool lit = (it->bit[0] >> state) & 1u;
    inst_add(s, orb_mesh(s), lit ? k->orb : k->orbOff, BF_EMITTER, x - 1.72f, 0.67f, z - f * 0.6f, 0.17f, 0.17f, 0.17f, 0.0f);
    if (lit) light_add(s, x - 1.72f, 0.9f, z - f * 0.6f, 1.6f, 1.15f, 0.65f);
    if (it->variant) {   /* two armchairs, facing the sofa */
        for (int side = -1; side <= 1; side += 2) {
            float cx = x + (float)side * 0.9f, cz = z + f * 1.05f, turn = (it->dir == 2 ? 0.0f : PI_F) + (float)side * 0.25f;
            box_local(s, e->leather, 0, cx, cz, turn, 0.0f, 0.22f, 0.0f, 0.68f, 0.3f, 0.62f, 0.0f);
            box_local(s, e->leather, 0, cx, cz, turn, 0.0f, 0.55f, 0.28f, 0.68f, 0.42f, 0.1f, 0.0f);
            box_local(s, e->leather, 0, cx, cz, turn, -0.31f, 0.42f, 0.0f, 0.08f, 0.2f, 0.6f, 0.0f);
            box_local(s, e->leather, 0, cx, cz, turn, 0.31f, 0.42f, 0.0f, 0.08f, 0.2f, 0.6f, 0.0f);
        }
    }
#undef ZB
}

/* A canvas facing dir (0 +z, 2 -z) on the wall at (ox, oz): a frame, and bands of colour. */
static void build_art(Scene *s, const Kit *k, const Extra *e, const FloorItem *it) {
    float n = it->dir == 0 ? 1.0f : -1.0f, w = it->a, h = it->b, cy = 1.55f, x = it->ox, z = it->oz;
#define SLAB(M, X0, Y0, X1, Y1, D0, D1) box(s, (M), 0, x + (X0), (Y0), fminf(z + n * (D0), z + n * (D1)), x + (X1), (Y1), fmaxf(z + n * (D0), z + n * (D1)))
    SLAB(k->frame, -w * 0.5f, cy - h * 0.5f, w * 0.5f, cy + h * 0.5f, 0.0f, 0.03f);
    SLAB(e->canvas, -w * 0.5f + 0.04f, cy - h * 0.5f + 0.04f, w * 0.5f - 0.04f, cy + h * 0.5f - 0.04f, 0.03f, 0.033f);
    int shapes = 3 + (int)(look(it->seed, 1u) * 3.0f);
    for (int i = 0; i < shapes; ++i) {
        uint32_t t = (uint32_t)i * 8u;
        float a = look(it->seed, 10u + t), b = look(it->seed, 11u + t), c = look(it->seed, 12u + t), d = look(it->seed, 13u + t);
        float iw = w - 0.08f, ih = h - 0.08f;
        float x0 = -iw * 0.5f + a * iw * 0.7f, x1 = fminf(x0 + iw * (0.1f + 0.5f * c), iw * 0.5f);
        float y0 = cy - ih * 0.5f + b * ih * 0.7f, y1 = fminf(y0 + ih * (0.1f + 0.5f * d), cy + ih * 0.5f);
        SLAB(e->ink[(int)(look(it->seed, 14u + t) * 4.99f)], x0, y0, x1, y1, 0.033f + 0.0015f * (float)i, 0.0345f + 0.0015f * (float)i);
    }
#undef SLAB
}

/* The cabinets, one a slot, stretched to the slot and the run's height; back to back in two rows where the plan has them. */
static void build_filing(Scene *s, const Kit *k, const Extra *e, const Furniture *f, const FloorItem *it) {
    const Piece *p = &f->cabinet;
    float h = it->variant ? 1.9f : 1.05f, zc = (it->z0 + it->z1) * 0.5f;
    int n = (int)floorf((it->x1 - it->x0) / 0.8f + 0.01f);
    float sx = 0.79f / (p->mx[0] - p->mn[0]), sy = h / (p->mx[1] - p->mn[1]), pd = p->mx[2] - p->mn[2];
    bool two = it->b > 0.5f;
    float depth = two ? (it->z1 - it->z0) * 0.5f - 0.005f : it->z1 - it->z0;
    for (int i = 0; i < n; ++i) {
        float x = it->x0 + 0.8f * (float)i + 0.395f;
        if (two) {
            piece_place(s, p, 0, 0, x, 0.0f, zc + depth * 0.5f + 0.005f, 0.0f, sx, sy, depth / pd);
            piece_place(s, p, 0, 0, x, 0.0f, zc - depth * 0.5f - 0.005f, PI_F, sx, sy, depth / pd);
        } else {
            piece_place(s, p, 0, 0, x, 0.0f, zc, it->dir == 2 ? PI_F : 0.0f, sx, sy, depth / pd);
        }
        if (!it->variant && look(it->seed, 20u + (uint32_t)i) < 0.5f) {   /* box files on the low ones */
            int nb = 3 + (int)(look(it->seed, 40u + (uint32_t)i) * 5.0f);
            for (int b = 0; b < nb; ++b)
                box(s, e->book[b & 3], 0, x - 0.31f + 0.075f * (float)b, h, zc - 0.13f, x - 0.245f + 0.075f * (float)b, h + 0.26f + 0.04f * look(it->seed, 60u + (uint32_t)b), zc + 0.13f);
        }
    }
    (void)k;
}

static void build_core(Scene *s, const Kit *k, const FloorItem *it, int state) {
    box(s, k->coreWall, 0, it->x0, -0.1f, it->z0, it->x1, s->ceilY + 0.1f, it->z1);
    if ((it->bit[0] >> state) & 1u) {
        box(s, k->coveOn, BF_EMITTER, it->x0, s->ceilY - 0.3f, it->z0 - 0.02f, it->x1, s->ceilY - 0.28f, it->z0);
    }
}

static void build_door(Scene *s, const Kit *k, const FloorItem *it) {
    float n = it->dir == 0 ? 1.0f : -1.0f, x = it->ox, z = it->oz, w = it->a * 0.5f;
#define D(M, X0, Y0, X1, Y1, A, B) box(s, (M), 0, (X0), (Y0), fminf(z + n * (A), z + n * (B)), (X1), (Y1), fmaxf(z + n * (A), z + n * (B)))
    D(k->door, x - w, 0.0f, x + w, 2.15f, -0.01f, 0.03f);
    D(k->frame, x - w - 0.05f, 0.0f, x - w, 2.2f, -0.01f, 0.05f);
    D(k->frame, x + w, 0.0f, x + w + 0.05f, 2.2f, -0.01f, 0.05f);
    D(k->frame, x - w - 0.05f, 2.15f, x + w + 0.05f, 2.2f, -0.01f, 0.05f);
#undef D
}

static void build_services(Scene *s, const Kit *k, const FloorPlan *P, float x0, float x1) {
    float c = s->ceilY, zEnd = P->zMax;
    for (int i = 0; i < P->ncolZ; ++i) box(s, k->concrete, 0, x0, c - 0.45f, P->colZ[i] - 0.2f, x1, c + 0.05f, P->colZ[i] + 0.2f);
    float db = P->ductBottom, dt = fminf(db + 0.45f, c - 0.02f);
    for (int i = 0; i < P->nduct; ++i) {
        float dx = P->ductX[i];
        box(s, k->duct, 0, dx - 0.35f, db, 0.4f, dx + 0.35f, dt, zEnd - 0.4f);
        for (float z = 3.0f; z < zEnd - 1.0f; z += 6.0f) box(s, k->duct, 0, dx - 0.2f, db - 0.1f, z - 0.2f, dx + 0.2f, db, z + 0.2f);
        /* its joints: a collar every 1.5 m, and a rod each side of every other one up to the slab */
        int j = 0;
        for (float z = 1.15f; z < zEnd - 0.5f; z += 1.5f, ++j) {
            box(s, k->duct, 0, dx - 0.365f, db - 0.015f, z - 0.02f, dx + 0.365f, dt + 0.015f, z + 0.02f);
            if (dt + 0.02f < c && (j & 1) == 0) {
                box(s, k->wire, 0, dx - 0.39f, db - 0.03f, z + 0.1f, dx + 0.39f, db - 0.01f, z + 0.12f);
                for (int side = -1; side <= 1; side += 2)
                    box(s, k->wire, 0, dx + 0.38f * (float)side - 0.005f, db - 0.03f, z + 0.105f, dx + 0.38f * (float)side + 0.005f, c, z + 0.115f);
            }
        }
    }
    for (int i = 0; i < P->ntray; ++i) box(s, k->tray, 0, x0, c - 0.42f, P->trayZ[i] - 0.18f, x1, c - 0.36f, P->trayZ[i] + 0.18f);
    for (float z = 1.5f; z < zEnd - 0.5f; z += 3.0f) box(s, k->pipe, 0, x0, c - 0.15f, z - 0.02f, x1, c - 0.11f, z + 0.02f);
}

bool floor_scene_build(Scene *s, const char *name, int state, const char *assets, bool *found) {
    const FloorEntry *en = NULL;
    for (int i = 0; i < FLOOR_CATALOGUE_COUNT && !en; ++i) if (!strcmp(FLOOR_CATALOGUE[i].name, name)) en = &FLOOR_CATALOGUE[i];
    *found = en != NULL;
    if (!en) return false;
    FloorPlan *P = malloc(sizeof(FloorPlan));
    if (!P) fatal("out of memory planning a floor");
    if (!floor_plan(&en->p, P)) {
        fprintf(stderr, "floor %s: %s\n", name, P->why);
        free(P);
        return false;
    }
    int st = state < 0 ? en->state : state;
    memset(s, 0, sizeof(*s));
    cube_mesh(s);
    float W = P->W, D = en->p.D;
    Kit k = kit_make(s, st, W, D);
    Extra e = extra_make(s, &k, en->p.style);
    Furniture fu;
    if (!furniture_load(s, &k, &fu, assets)) { free(P); return false; }
    scene_defaults(s, name, en->p.corner ? MOD_CORNER : en->p.core == FLOOR_CORE_BACK ? MOD_CORE : MOD_THROUGH, W, D);
    s->H = en->p.H;
    s->ceilY = P->ceil;
    s->head = P->ceil - 0.05f;
    s->bays = en->p.bays;
    s->bayW = en->p.bayW;
    if (en->p.corner) { s->period = 0.0f; s->xmin = 0.0f; }
    s->view[0] = P->view.x; s->view[1] = P->view.z; s->view[2] = P->viewYaw;
    snprintf(s->floorInfo, sizeof(s->floorInfo), "\"style\": \"%s\", \"core\": \"%s\", \"seed\": %u, \"attempts\": %d, \"layout\": \"%016llx\"",
             FLOOR_STYLE_NAMES[en->p.style], FLOOR_CORE_NAMES[en->p.core], en->p.seed, P->attempts, (unsigned long long)floor_hash(P));

    float x0 = en->p.corner ? 0.0f : -0.5f, x1 = en->p.corner ? P->xMax + 1.5f : W + 0.5f;
    slabs(s, &k, x0, x1);
    build_services(s, &k, P, en->p.corner ? 0.0f : -0.5f, en->p.corner ? P->xMax : W + 0.5f);
    /* the far facade's mullions, on the half-bay so a tile's seam can't show */
    if (en->p.core != FLOOR_CORE_BACK) {
        float step = en->p.bayW * 0.5f, z = D;
        for (float x = 0.0f; x < W - 0.01f; x += step) box(s, k.mullion, 0, x - 0.03f, 0.0f, z - 0.12f, x + 0.03f, s->ceilY, z - 0.02f);
        box(s, k.mullion, 0, -0.5f, 0.0f, z - 0.12f, W + 0.5f, 0.08f, z - 0.02f);
        box(s, k.mullion, 0, -0.5f, s->ceilY - 0.1f, z - 0.12f, W + 0.5f, s->ceilY, z - 0.02f);
    }
    if (en->p.corner) {   /* the side glass */
        for (float z = 0.0f; z < P->zMax; z += 1.5f) box(s, k.mullion, 0, 0.02f, 0.0f, z - 0.03f, 0.12f, s->ceilY, z + 0.03f);
        box(s, k.mullion, 0, 0.02f, 0.0f, 0.0f, 0.12f, 0.08f, P->zMax);
    }
    float pendantY = fminf(2.62f, P->ceil - 0.78f), trofferY = P->ceil - 0.6f;
    for (int i = 0; i < P->nitem; ++i) {
        const FloorItem *it = &P->item[i];
        switch (it->kind) {
        case FI_COLUMN:  box(s, k.concrete, 0, it->x0, -0.1f, it->z0, it->x1, s->ceilY + 0.1f, it->z1); break;
        case FI_CORE:    build_core(s, &k, it, st); break;
        case FI_DOOR:    build_door(s, &k, it); break;
        case FI_DESK:    build_desk(s, &k, &e, &fu, it, st); break;
        case FI_PANEL:   build_panel(s, &k, &fu, it, st); break;
        case FI_PENDANT: build_pendant(s, &k, it, pendantY, st); break;
        case FI_TROFFER:
            inst_add(s, fu.card, k.trofferOff, BF_EMITTER, it->ox, trofferY + 0.04f, it->oz, it->x1 - it->x0, 0.08f, it->z1 - it->z0, 0.0f);
            if ((it->bit[0] >> st) & 1u) inst_add(s, fu.card, k.trofferOn, BF_EMITTER, it->ox, trofferY - 0.002f, it->oz, 1.2f, 0.004f, 1.2f, 0.0f);
            break;
        case FI_ROOM:    build_room(s, &k, &fu, it, st); break;
        case FI_LOUNGE:  build_lounge(s, &k, &e, it, assets, st); break;
        case FI_WALL:
            box(s, e.panel, 0, it->x0, 0.0f, it->z0, it->x1, it->b, it->z1);
            if ((it->bit[0] >> st) & 1u) box(s, k.coveOn, BF_EMITTER, it->x0, it->b, it->z0, it->x1, it->b + 0.004f, it->z1);
            break;
        case FI_ART:     build_art(s, &k, &e, it); break;
        case FI_FILING:  build_filing(s, &k, &e, &fu, it); break;
        case FI_PRINTER:
            box(s, k.pedestal, 0, it->x0, 0.0f, it->z0, it->x1, 0.95f, it->z1);
            box(s, e.pedLight, 0, it->x0 + 0.03f, 0.95f, it->z0 + 0.03f, it->x1 - 0.03f, 1.1f, it->z1 - 0.03f);
            break;
        case FI_PLANT:   plant(s, &k, it->ox, it->oz, it->variant, it->a, it->seed); break;
        default: break;
        }
    }
    if (st != en->state) {
        size_t len = strlen(s->name);
        snprintf(s->name + len, sizeof(s->name) - len, "%s", LIGHT_STATE_SUFFIX[st]);
    }
    s->state = st;
    printf("floor %s (%s, %s): %d x %.1f m, %.1f deep, %d items, attempt %d\n", s->name, LIGHT_STATE_NAMES[st], FLOOR_STYLE_NAMES[en->p.style], en->p.bays, en->p.bayW, D, P->nitem, P->attempts);
    printf("scene %s: %d instances, %d meshes, %u triangles, %d lights\n", s->name, s->ninsts, s->nmeshes, s->nidx / 3, s->nlights);
    free(P);
    return true;
}
