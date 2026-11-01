#include "kit.h"
#include "floor_gen.h"
#include "plant.h"

/* A planned floor (floor_gen.c) as a scene: each item built from the kit's boxes and meshes, its
 * lights on as the module's light state says. The furniture is stand-ins until the user's models
 * arrive (IM6); its materials are still set here and in kit.c. */

/* What a generated floor adds to the kit. */
typedef struct Extra {
    int pedLight, paper, mug, book[4], canvas, ink[5], panel, leather, rugs[3], handle;
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
    e.paper = mat_add(s, 0.8f, 0.78f, 0.72f, 0, 0, 0);
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
    e.handle = mat_add(s, 0.5f, 0.5f, 0.5f, 0, 0, 0);
    mat_surface(s, e.handle, 0.35f, 1.0f, 0.0f, 0.0f);
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

static void build_desk(Scene *s, const Kit *k, const Extra *e, const FloorItem *it, int state) {
    float yaw = yaw_of(it->dir), w = it->a, ox = it->ox, oz = it->oz;
    uint32_t sd = it->seed;
#define B(M, F, X, Y, Z, SX, SY, SZ) box_local(s, (M), (F), ox, oz, yaw, (X), (Y), (Z), (SX), (SY), (SZ), 0.0f)
    B(k->wood, 0, 0.0f, 0.735f, 0.4f, w, 0.03f, 0.8f);
    B(k->frame, 0, -w * 0.5f + 0.035f, 0.36f, 0.4f, 0.03f, 0.72f, 0.7f);
    B(k->frame, 0, w * 0.5f - 0.035f, 0.36f, 0.4f, 0.03f, 0.72f, 0.7f);
    B(k->frame, 0, 0.0f, 0.51f, 0.07f, w - 0.1f, 0.42f, 0.02f);
    /* a pedestal: pale on some floors, with its three drawers' gaps and handles on the side you sit */
    float side = look(sd, 1u) < 0.5f ? 1.0f : -1.0f, px = side * (w * 0.5f - 0.29f);
    int ped = look(it->seed & 0xFFFFFF00u, 2u) < 0.35f ? e->pedLight : k->pedestal;
    B(ped, 0, px, 0.3f, 0.425f, 0.42f, 0.6f, 0.55f);
    for (int d = 1; d <= 2; ++d) B(k->frame, 0, px, (float)d * 0.2f, 0.702f, 0.40f, 0.006f, 0.006f);
    for (int d = 0; d < 3; ++d) B(e->handle, 0, px, 0.14f + (float)d * 0.2f, 0.708f, 0.12f, 0.012f, 0.012f);
    if (it->variant > 0) {
        /* the screens, on stands, facing their user; only the panel emits */
        for (int i = 0; i < it->variant; ++i) {
            float cx = it->variant == 1 ? 0.0f : -0.3f + 0.6f * (float)i;
            bool on = (it->bit[i] >> state) & 1u;
            B(k->plastic, 0, cx, 1.08f, 0.17f, 0.54f, 0.32f, 0.03f);
            B(on ? k->screenOn : k->screenOff, 0, cx, 1.08f, 0.187f, 0.51f, 0.29f, 0.004f);
            B(k->plastic, 0, cx, 0.84f, 0.15f, 0.05f, 0.18f, 0.05f);
            B(k->plastic, 0, cx, 0.755f, 0.15f, 0.2f, 0.01f, 0.14f);
        }
        B(k->plastic, 0, 0.0f, 0.76f, 0.5f, 0.45f, 0.02f, 0.14f);      /* keyboard */
        B(k->plastic, 0, 0.3f, 0.76f, 0.5f, 0.06f, 0.02f, 0.1f);       /* mouse */
    }
    if ((it->bit[3] >> state) & 1u) B(k->stripOn, BF_EMITTER, 0.0f, 0.714f, 0.12f, w - 0.2f, 0.004f, 0.015f);
    if (it->b != 0.0f) {   /* a task lamp */
        float lx = it->b * w * 0.4f, hd = lx - it->b * 0.09f;
        bool on = (it->bit[2] >> state) & 1u;
        B(k->plastic, 0, lx, 0.76f, 0.15f, 0.12f, 0.02f, 0.12f);
        B(k->plastic, 0, lx, 0.97f, 0.15f, 0.02f, 0.4f, 0.02f);
        B(k->lampOff, BF_EMITTER, hd, 1.145f, 0.15f, 0.22f, 0.05f, 0.08f);
        if (on) {
            B(k->lampOn, BF_EMITTER, hd, 1.118f, 0.15f, 0.18f, 0.004f, 0.06f);
            float c = cosf(yaw), n = sinf(yaw);
            light_add(s, ox + c * hd + n * 0.15f, 1.1f, oz - n * hd + c * 0.15f, 0.9f, 0.52f, 0.22f);
        }
    }
    /* what people leave out: a stack of books, papers, a mug */
    if (it->variant > 0) {
        float bx = -side * 0.0f + (it->b != 0.0f ? -it->b * w * 0.36f : -side * w * 0.36f);
        if (look(sd, 3u) < 0.45f) {
            int books = 2 + (int)(look(sd, 4u) * 3.0f);
            for (int i = 0; i < books; ++i)
                box_local(s, e->book[(int)(look(sd, 10u + (uint32_t)i) * 3.99f)], 0, ox, oz, yaw, bx, 0.77f + 0.036f * (float)i, 0.28f,
                          0.20f + 0.04f * look(sd, 20u + (uint32_t)i), 0.034f, 0.27f, (look(sd, 30u + (uint32_t)i) - 0.5f) * 0.5f);
        }
        if (look(sd, 5u) < 0.5f) B(e->paper, 0, 0.45f * (look(sd, 6u) - 0.5f) * w, 0.752f, 0.6f, 0.21f, 0.002f, 0.3f);
        if (look(sd, 7u) < 0.6f) {
            float mx = (look(sd, 8u) < 0.5f ? 0.42f : -0.42f) * w * 0.8f, c = cosf(yaw), n = sinf(yaw);
            inst_add(s, e->cyl, e->mug, 0, ox + c * mx + n * 0.55f, 0.75f, oz - n * mx + c * 0.55f, 0.04f, 0.095f, 0.04f, 0.0f);
        }
    }
    /* the chair, somewhere near its desk */
    float jx = (look(sd, 9u) - 0.5f) * 0.35f, sp = (look(sd, 40u) - 0.5f) * 0.9f, c = cosf(yaw), n = sinf(yaw), lz = 1.1f;
    chair(s, k, ox + c * jx + n * lz, oz - n * jx + c * lz, yaw + sp);
#undef B
}

static void build_panel(Scene *s, const Kit *k, const FloorItem *it, int state) {
    box(s, k->board, 0, it->x0, 0.75f, it->z0, it->x1, 1.15f, it->z1);
    if ((it->bit[0] >> state) & 1u) box(s, k->coveOn, BF_EMITTER, it->x0, 1.15f, it->z0, it->x1, 1.154f, it->z1);
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

static void build_room(Scene *s, const Kit *k, const FloorItem *it, int state) {
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
        chair(s, k, x, t.z0 - 0.45f, (look(it->seed, 100u + i) - 0.5f) * 0.5f);
        chair(s, k, x, t.z1 + 0.45f, PI_F + (look(it->seed, 200u + i) - 0.5f) * 0.5f);
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

static void build_filing(Scene *s, const Kit *k, const Extra *e, const FloorItem *it) {
    float h = it->variant ? 1.9f : 1.05f, zc = (it->z0 + it->z1) * 0.5f;
    int drawers = it->variant ? 5 : 3, n = (int)floorf((it->x1 - it->x0) / 0.8f + 0.01f), mat = look(it->seed, 1u) < 0.5f ? e->pedLight : k->pedestal;
    for (int i = 0; i < n; ++i) {
        float x0 = it->x0 + 0.8f * (float)i, x1 = x0 + 0.79f;
        box(s, mat, 0, x0, 0.0f, it->z0, x1, h, it->z1);
        for (int side = 0; side < (it->b > 0.5f ? 2 : 1); ++side) {
            bool nearFace = it->b > 0.5f ? side == 0 : it->dir == 2;
            float z = nearFace ? it->z0 : it->z1, sgn = nearFace ? -1.0f : 1.0f;
            for (int d = 0; d < drawers; ++d) {
                float y = (float)(d + 1) * h / (float)drawers;
                if (d < drawers - 1) box(s, k->frame, 0, x0 + 0.01f, y - 0.004f, fminf(z, z + sgn * 0.004f), x1 - 0.01f, y + 0.004f, fmaxf(z, z + sgn * 0.004f));
                box(s, e->handle, 0, x0 + 0.3f, y - h / (float)drawers * 0.5f, fminf(z, z + sgn * 0.02f), x1 - 0.3f, y - h / (float)drawers * 0.5f + 0.015f, fmaxf(z, z + sgn * 0.02f));
            }
        }
        if (!it->variant && look(it->seed, 20u + (uint32_t)i) < 0.5f) {   /* box files on the low ones */
            int nb = 3 + (int)(look(it->seed, 40u + (uint32_t)i) * 5.0f);
            for (int b = 0; b < nb; ++b)
                box(s, e->book[b & 3], 0, x0 + 0.08f + 0.075f * (float)b, h, zc - 0.13f, x0 + 0.145f + 0.075f * (float)b, h + 0.26f + 0.04f * look(it->seed, 60u + (uint32_t)b), zc + 0.13f);
        }
    }
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
        case FI_DESK:    build_desk(s, &k, &e, it, st); break;
        case FI_PANEL:   build_panel(s, &k, it, st); break;
        case FI_PENDANT: build_pendant(s, &k, it, pendantY, st); break;
        case FI_TROFFER:
            box(s, k.trofferOff, BF_EMITTER, it->x0, trofferY, it->z0, it->x1, trofferY + 0.08f, it->z1);
            if ((it->bit[0] >> st) & 1u) box(s, k.trofferOn, BF_EMITTER, it->x0 + 0.02f, trofferY - 0.004f, it->z0 + 0.02f, it->x1 - 0.02f, trofferY, it->z1 - 0.02f);
            break;
        case FI_ROOM:    build_room(s, &k, it, st); break;
        case FI_LOUNGE:  build_lounge(s, &k, &e, it, assets, st); break;
        case FI_WALL:
            box(s, e.panel, 0, it->x0, 0.0f, it->z0, it->x1, it->b, it->z1);
            if ((it->bit[0] >> st) & 1u) box(s, k.coveOn, BF_EMITTER, it->x0, it->b, it->z0, it->x1, it->b + 0.004f, it->z1);
            break;
        case FI_ART:     build_art(s, &k, &e, it); break;
        case FI_FILING:  build_filing(s, &k, &e, it); break;
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
