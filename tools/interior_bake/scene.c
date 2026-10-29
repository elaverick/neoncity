#include "bake.h"
#include "models.h"
#include "material_def.h"
#include "plant.h"

/* The IM1 test modules, after the reference (docs/reference/office_night.png):
 * an exposed ceiling with beams, ducts, trays and sprinkler runs, concrete
 * columns on a grid, desk pairs with pedestals, chairs, monitors and lamps,
 * linear pendants on thin wires, glass meeting rooms and a lounge with the
 * engine's sofa and a lobby plant. Built from boxes (one unit cube, instanced)
 * plus those two meshes, deliberately mixing large planes with centimetre
 * parts (wires, sprinkler pipes, a rug, the manifestation band on the glass)
 * so the spike sees what the representation drops.
 *
 *   through30  12 m wide, 30 m deep to far glass: the deep open floor
 *   core12     12 m deep to a core wall with doors
 *   corner16   a corner: glass along the left side too, 16 m to a core wall
 *   glass20    20 m through, with two glass meeting rooms and a lounge
 */

const char *const SCENE_NAMES[] = { "through30", "core12", "corner16", "glass20" };
const int SCENE_COUNT = 4;

#define GROW(arr, n, cap, extra) do { \
    if ((n) + (extra) > (cap)) { \
        (cap) = ((n) + (extra)) * 2; \
        (arr) = realloc((arr), sizeof(*(arr)) * (size_t)(cap)); \
        if (!(arr)) fatal("out of memory building a scene"); \
    } } while (0)

static float rnd(uint32_t *s) {   /* xorshift32, one draw a statement */
    *s ^= *s << 13; *s ^= *s >> 17; *s ^= *s << 5;
    return (*s >> 8) * (1.0f / 16777216.0f);
}

static int mesh_add(Scene *s, const float *v, uint32_t nv, const uint32_t *ix, uint32_t ni) {
    GROW(s->verts, s->nverts * 3, s->capVerts, nv * 3);
    GROW(s->idx, s->nidx, s->capIdx, ni);
    GROW(s->meshes, s->nmeshes, s->capMeshes, 1);
    BakeMesh *m = &s->meshes[s->nmeshes];
    m->firstIndex = s->nidx; m->indexCount = ni; m->baseVertex = s->nverts; m->vertexCount = nv;
    memcpy(s->verts + s->nverts * 3, v, sizeof(float) * 3 * nv);
    memcpy(s->idx + s->nidx, ix, sizeof(uint32_t) * ni);
    s->nverts += nv;
    s->nidx += ni;
    return s->nmeshes++;
}

static int mat_add(Scene *s, float r, float g, float b, float er, float eg, float eb) {
    GROW(s->mats, s->nmats, s->capMats, 1);
    BakeMat *m = &s->mats[s->nmats];
    memset(m, 0, sizeof(*m));
    m->albedo[0] = r; m->albedo[1] = g; m->albedo[2] = b;
    m->emission[0] = er; m->emission[1] = eg; m->emission[2] = eb;
    return s->nmats++;
}

static void light_add(Scene *s, float x, float y, float z, float r, float g, float b) {
    GROW(s->lights, s->nlights, s->capLights, 1);
    BakeLight *l = &s->lights[s->nlights++];
    memset(l, 0, sizeof(*l));
    l->pos[0] = x; l->pos[1] = y; l->pos[2] = z;
    l->power[0] = r; l->power[1] = g; l->power[2] = b;
}

/* An instance of mesh scaled by (sx, sy, sz), turned by yaw about y, centred at c. */
static void inst_add(Scene *s, int mesh, int mat, uint32_t flags, float cx, float cy, float cz,
                     float sx, float sy, float sz, float yaw) {
    GROW(s->insts, s->ninsts, s->capInsts, 1);
    BakeInst *in = &s->insts[s->ninsts++];
    float c = cosf(yaw), n = sinf(yaw);
    in->mesh = (uint32_t)mesh; in->mat = (uint32_t)mat; in->flags = flags;
    float xf[12] = { c * sx, 0, n * sz, cx,   0, sy, 0, cy,   -n * sx, 0, c * sz, cz };
    memcpy(in->xf, xf, sizeof(xf));
}

static void box(Scene *s, int mat, uint32_t flags, float x0, float y0, float z0, float x1, float y1, float z1) {
    inst_add(s, s->cube, mat, flags, (x0 + x1) * 0.5f, (y0 + y1) * 0.5f, (z0 + z1) * 0.5f, x1 - x0, y1 - y0, z1 - z0, 0.0f);
}

/* A box at (lx, ly, lz) in a frame at (ox, oz) turned by yaw. */
static void box_local(Scene *s, int mat, uint32_t flags, float ox, float oz, float yaw,
                      float lx, float ly, float lz, float sx, float sy, float sz, float spin) {
    float c = cosf(yaw), n = sinf(yaw);
    inst_add(s, s->cube, mat, flags, ox + c * lx + n * lz, ly, oz - n * lx + c * lz, sx, sy, sz, yaw + spin);
}

static void cube_mesh(Scene *s) {
    float v[24];
    for (int i = 0; i < 8; ++i) {
        v[i * 3 + 0] = (float)(i & 1) - 0.5f;
        v[i * 3 + 1] = (float)((i >> 1) & 1) - 0.5f;
        v[i * 3 + 2] = (float)((i >> 2) & 1) - 0.5f;
    }
    static const uint32_t q[6][4] = { { 0, 4, 6, 2 }, { 1, 3, 7, 5 }, { 0, 1, 5, 4 }, { 2, 6, 7, 3 }, { 0, 2, 3, 1 }, { 4, 5, 7, 6 } };
    uint32_t ix[36];
    for (int f = 0; f < 6; ++f) {
        uint32_t t[6] = { q[f][0], q[f][1], q[f][2], q[f][0], q[f][2], q[f][3] };
        memcpy(ix + f * 6, t, sizeof(t));
    }
    s->cube = mesh_add(s, v, 8, ix, 36);   /* counter-clockwise seen from outside */
}

/* ------------------------------------------------------------- the kit --- */

typedef struct Kit {
    int floor, ceiling, concrete, duct, tray, pipe, wood, frame, pedestal, mesh, plastic, screenOn, screenOff;
    int pendantOn, pendantOff, lampOn, lampOff, wire, mullion, coreWall, door, board, glass, band, rug, marble, orb, pot, leaf, bark, sofa;
} Kit;

static Kit kit_make(Scene *s) {
    Kit k;
    k.floor      = mat_add(s, 0.16f, 0.155f, 0.15f, 0, 0, 0);    /* polished concrete */
    k.ceiling    = mat_add(s, 0.07f, 0.07f, 0.075f, 0, 0, 0);    /* painted-out exposed slab */
    k.concrete   = mat_add(s, 0.26f, 0.25f, 0.24f, 0, 0, 0);
    k.duct       = mat_add(s, 0.32f, 0.33f, 0.35f, 0, 0, 0);
    k.tray       = mat_add(s, 0.22f, 0.22f, 0.23f, 0, 0, 0);
    k.pipe       = mat_add(s, 0.35f, 0.05f, 0.04f, 0, 0, 0);     /* sprinkler red */
    k.wood       = mat_add(s, 0.07f, 0.05f, 0.035f, 0, 0, 0);
    k.frame      = mat_add(s, 0.03f, 0.03f, 0.03f, 0, 0, 0);
    k.pedestal   = mat_add(s, 0.045f, 0.045f, 0.05f, 0, 0, 0);
    k.mesh       = mat_add(s, 0.025f, 0.025f, 0.028f, 0, 0, 0);
    k.plastic    = mat_add(s, 0.02f, 0.02f, 0.022f, 0, 0, 0);
    k.screenOn   = mat_add(s, 0.02f, 0.02f, 0.02f, 0.10f, 0.22f, 0.30f);
    k.screenOff  = mat_add(s, 0.01f, 0.01f, 0.012f, 0, 0, 0);
    k.pendantOn  = mat_add(s, 0.05f, 0.05f, 0.05f, 4.0f, 3.0f, 1.9f);
    k.pendantOff = mat_add(s, 0.05f, 0.05f, 0.05f, 0, 0, 0);
    k.lampOn     = mat_add(s, 0.05f, 0.04f, 0.03f, 6.0f, 3.4f, 1.2f);
    k.lampOff    = mat_add(s, 0.05f, 0.04f, 0.03f, 0, 0, 0);
    k.wire       = mat_add(s, 0.02f, 0.02f, 0.02f, 0, 0, 0);
    k.mullion    = mat_add(s, 0.04f, 0.04f, 0.045f, 0, 0, 0);
    k.coreWall   = mat_add(s, 0.30f, 0.29f, 0.27f, 0, 0, 0);
    k.door       = mat_add(s, 0.10f, 0.07f, 0.05f, 0, 0, 0);
    k.board      = mat_add(s, 0.45f, 0.42f, 0.30f, 0, 0, 0);
    k.glass      = mat_add(s, 0.0f, 0.0f, 0.0f, 0, 0, 0);
    k.band       = mat_add(s, 0.60f, 0.60f, 0.60f, 0, 0, 0);    /* frosted manifestation */
    k.rug        = mat_add(s, 0.16f, 0.09f, 0.06f, 0, 0, 0);
    k.marble     = mat_add(s, 0.70f, 0.68f, 0.64f, 0, 0, 0);
    k.orb        = mat_add(s, 0.8f, 0.8f, 0.8f, 3.0f, 2.2f, 1.3f);
    k.pot        = mat_add(s, 0.06f, 0.06f, 0.065f, 0, 0, 0);
    k.leaf       = mat_add(s, 0.05f, 0.11f, 0.035f, 0, 0, 0);
    k.bark       = mat_add(s, 0.12f, 0.08f, 0.05f, 0, 0, 0);
    k.sofa       = mat_add(s, 0.10f, 0.08f, 0.07f, 0, 0, 0);    /* replaced by the model's material if it loads */
    return k;
}

static void slabs(Scene *s, const Kit *k, float x0, float x1) {
    box(s, k->floor, 0, x0, -2.0f, -2.0f, x1, 0.0f, s->D + 2.0f);
    box(s, k->ceiling, 0, x0, s->ceilY, -2.0f, x1, s->ceilY + 2.6f, s->D + 2.0f);
}

static void column(Scene *s, const Kit *k, float x, float z) {
    box(s, k->concrete, 0, x - 0.3f, -0.1f, z - 0.3f, x + 0.3f, s->ceilY + 0.1f, z + 0.3f);
}

/* Downstand beams on the column lines (along x), ducts along z, trays and sprinkler runs along x. */
static void services(Scene *s, const Kit *k, float x0, float x1, const float *beamZ, int nbeam, const float *ductX, int nduct,
                     const float *trayZ, int ntray, float zEnd) {
    float c = s->ceilY;
    for (int i = 0; i < nbeam; ++i) box(s, k->concrete, 0, x0, c - 0.45f, beamZ[i] - 0.2f, x1, c + 0.05f, beamZ[i] + 0.2f);
    for (int i = 0; i < nduct; ++i) {
        box(s, k->duct, 0, ductX[i] - 0.35f, c - 0.95f, 0.4f, ductX[i] + 0.35f, c - 0.5f, zEnd - 0.4f);
        for (float z = 3.0f; z < zEnd - 1.0f; z += 6.0f)   /* diffusers */
            box(s, k->duct, 0, ductX[i] - 0.2f, c - 1.05f, z - 0.2f, ductX[i] + 0.2f, c - 0.95f, z + 0.2f);
    }
    for (int i = 0; i < ntray; ++i) box(s, k->tray, 0, x0, c - 0.42f, trayZ[i] - 0.18f, x1, c - 0.36f, trayZ[i] + 0.18f);
    for (float z = 1.5f; z < zEnd - 0.5f; z += 3.0f) box(s, k->pipe, 0, x0, c - 0.15f, z - 0.02f, x1, c - 0.11f, z + 0.02f);
}

/* A 1.6 m linear pendant centred at (x, z) on two wires. */
static void pendant(Scene *s, const Kit *k, float x, float z, bool on) {
    float y = 2.62f;
    box(s, on ? k->pendantOn : k->pendantOff, BF_EMITTER, x - 0.8f, y, z - 0.06f, x + 0.8f, y + 0.05f, z + 0.06f);
    box(s, k->wire, 0, x - 0.7f, y + 0.05f, z - 0.004f, x - 0.692f, s->ceilY, z + 0.004f);
    box(s, k->wire, 0, x + 0.692f, y + 0.05f, z - 0.004f, x + 0.7f, s->ceilY, z + 0.004f);
    if (on) light_add(s, x, y - 0.03f, z, 9.0f, 6.8f, 4.3f);
}

static void chair(Scene *s, const Kit *k, float x, float z, float yaw) {
    box_local(s, k->mesh, 0, x, z, yaw, 0, 0.47f, 0, 0.5f, 0.07f, 0.48f, 0);
    box_local(s, k->mesh, 0, x, z, yaw, 0, 0.82f, 0.26f, 0.48f, 0.55f, 0.05f, 0);
    box_local(s, k->frame, 0, x, z, yaw, -0.27f, 0.64f, 0.02f, 0.04f, 0.04f, 0.36f, 0);
    box_local(s, k->frame, 0, x, z, yaw, 0.27f, 0.64f, 0.02f, 0.04f, 0.04f, 0.36f, 0);
    box_local(s, k->frame, 0, x, z, yaw, 0, 0.25f, 0, 0.06f, 0.38f, 0.06f, 0);
    box_local(s, k->frame, 0, x, z, yaw, 0, 0.05f, 0, 0.62f, 0.04f, 0.06f, 0.6f);
    box_local(s, k->frame, 0, x, z, yaw, 0, 0.05f, 0, 0.62f, 0.04f, 0.06f, 2.17f);
}

/* A desk from x to x + 1.6, its back edge at zBack, running 0.8 m in direction dir (+1 or -1), its user beyond. */
static void desk(Scene *s, const Kit *k, float x, float zBack, float dir, uint32_t *rng) {
    float zf = zBack + dir * 0.8f, za = fminf(zBack, zf), zb = fmaxf(zBack, zf);
    box(s, k->wood, 0, x, 0.72f, za, x + 1.6f, 0.75f, zb);
    box(s, k->frame, 0, x + 0.02f, 0.0f, za + 0.05f, x + 0.05f, 0.72f, zb - 0.05f);
    box(s, k->frame, 0, x + 1.55f, 0.0f, za + 0.05f, x + 1.58f, 0.72f, zb - 0.05f);
    float zm = zBack + dir * 0.06f;
    box(s, k->frame, 0, x + 0.05f, 0.3f, fminf(zm, zm + dir * 0.02f), x + 1.55f, 0.72f, fmaxf(zm, zm + dir * 0.02f));
    float zp0 = zBack + dir * 0.15f, zp1 = zBack + dir * 0.7f;
    box(s, k->pedestal, 0, x + 1.1f, 0.0f, fminf(zp0, zp1), x + 1.52f, 0.6f, fmaxf(zp0, zp1));
    int screens = rnd(rng) < 0.4f ? 2 : 1;
    for (int i = 0; i < screens; ++i) {
        float cx = screens == 1 ? x + 0.8f : x + 0.5f + 0.6f * (float)i;
        bool on = rnd(rng) < 0.6f;
        float zs = zBack + dir * 0.18f;
        box(s, on ? k->screenOn : k->screenOff, 0, cx - 0.27f, 0.92f, zs - 0.012f, cx + 0.27f, 1.24f, zs + 0.012f);
        box(s, k->plastic, 0, cx - 0.025f, 0.75f, zs - 0.025f + dir * 0.03f, cx + 0.025f, 0.93f, zs + 0.025f + dir * 0.03f);
        box(s, k->plastic, 0, cx - 0.1f, 0.75f, zs - 0.07f + dir * 0.03f, cx + 0.1f, 0.76f, zs + 0.07f + dir * 0.03f);
    }
    float zk = zBack + dir * 0.5f;
    box(s, k->plastic, 0, x + 0.55f, 0.75f, zk - 0.07f, x + 1.0f, 0.77f, zk + 0.07f);
    if (rnd(rng) < 0.35f) {   /* a task lamp */
        bool on = rnd(rng) < 0.7f;
        float lx = x + 0.15f, lz = zBack + dir * 0.15f;
        box(s, k->plastic, 0, lx - 0.06f, 0.75f, lz - 0.06f, lx + 0.06f, 0.77f, lz + 0.06f);
        box(s, k->plastic, 0, lx - 0.01f, 0.77f, lz - 0.01f, lx + 0.01f, 1.17f, lz + 0.01f);
        box(s, on ? k->lampOn : k->lampOff, BF_EMITTER, lx - 0.02f, 1.12f, lz - 0.04f, lx + 0.2f, 1.17f, lz + 0.04f);
        if (on) light_add(s, lx + 0.12f, 1.1f, lz, 0.9f, 0.52f, 0.22f);
    }
    float jitter = (rnd(rng) - 0.5f) * 0.4f;
    float yaw = (dir > 0 ? 0.0f : 3.14159265f) + (rnd(rng) - 0.5f) * 0.9f;
    chair(s, k, x + 0.8f + jitter, zBack + dir * 1.15f, yaw);
}

/* Desks back to back along zc, in runs of 1.6 m from each xs[i] for n[i] desks, with a pendant over each pair. */
static void desk_rows(Scene *s, const Kit *k, float zc, const float *xs, const int *n, int runs, uint32_t *rng) {
    for (int r = 0; r < runs; ++r) {
        float x0 = xs[r], x1 = xs[r] + 1.6f * (float)n[r];
        box(s, k->board, 0, x0 + 0.05f, 0.75f, zc - 0.01f, x1 - 0.05f, 1.15f, zc + 0.01f);   /* the screen between */
        for (int i = 0; i < n[r]; ++i) {
            float x = x0 + 1.6f * (float)i;
            desk(s, k, x, zc - 0.01f, -1.0f, rng);
            desk(s, k, x, zc + 0.01f, 1.0f, rng);
            pendant(s, k, x + 0.8f, zc, rnd(rng) < 0.45f);
        }
    }
}

/* Mullions and transoms of glass at depth z (the far facade). */
static void far_glass(Scene *s, const Kit *k, float x0, float x1, float z) {
    for (float x = x0; x < x1 - 0.01f; x += 1.5f) box(s, k->mullion, 0, x - 0.03f, 0.0f, z - 0.12f, x + 0.03f, s->ceilY, z - 0.02f);
    box(s, k->mullion, 0, x0 - 0.5f, 0.0f, z - 0.12f, x1 + 0.5f, 0.08f, z - 0.02f);
    box(s, k->mullion, 0, x0 - 0.5f, s->ceilY - 0.1f, z - 0.12f, x1 + 0.5f, s->ceilY, z - 0.02f);
}

static void core_wall(Scene *s, const Kit *k, float x0, float x1, float z, uint32_t *rng) {
    box(s, k->coreWall, 0, x0, -0.1f, z, x1, s->ceilY + 0.1f, z + 2.0f);
    for (float x = x0 + 1.5f + rnd(rng) * 2.0f; x < x1 - 2.0f; x += 4.0f + rnd(rng) * 3.0f) {
        box(s, k->door, 0, x, 0.0f, z - 0.03f, x + 0.95f, 2.15f, z + 0.01f);
        box(s, k->frame, 0, x - 0.05f, 0.0f, z - 0.05f, x, 2.2f, z + 0.01f);
        box(s, k->frame, 0, x + 0.95f, 0.0f, z - 0.05f, x + 1.0f, 2.2f, z + 0.01f);
        box(s, k->frame, 0, x - 0.05f, 2.15f, z - 0.05f, x + 1.0f, 2.2f, z + 0.01f);
    }
    float xb = x0 + (x1 - x0) * 0.55f;
    box(s, k->board, 0, xb, 1.0f, z - 0.02f, xb + 1.2f, 1.9f, z);
    box(s, k->pedestal, 0, xb + 1.6f, 0.0f, z - 0.55f, xb + 2.2f, 1.05f, z);   /* a printer */
}

static void plant(Scene *s, const Kit *k, float x, float z, int species, float height, uint32_t seed) {
    box(s, k->pot, 0, x - 0.24f, 0.0f, z - 0.24f, x + 0.24f, 0.45f, z + 0.24f);
    PlantParams p;
    plant_defaults(&p, species, seed);
    p.height = height;
    PlantMesh m = { 0 };
    if (!plant_build(&p, &m)) { plant_mesh_free(&m); return; }
    for (int part = 0; part < 2; ++part) {   /* leaves and stems: open surfaces */
        float *v = malloc(sizeof(float) * 3 * m.count);
        uint32_t *ix = malloc(sizeof(uint32_t) * m.count), n = 0;
        if (!v || !ix) fatal("out of memory building a plant");
        for (uint32_t i = 0; i + 2 < m.count; i += 3) {
            if ((m.v[i].part == PP_LEAF) != (part == 0)) continue;
            for (int j = 0; j < 3; ++j) {
                memcpy(v + n * 3, m.v[i + j].p, sizeof(float) * 3);
                ix[n] = n;
                n++;
            }
        }
        if (n) inst_add(s, mesh_add(s, v, n, ix, n), part == 0 ? k->leaf : k->bark, BF_OPEN, x, 0.42f, z, 1, 1, 1, seed * 0.37f);
        free(v);
        free(ix);
    }
    plant_mesh_free(&m);
}

/* The engine's sofa (assets/models/sofa.glb), standing at (x, z) facing yaw; a box if it isn't there. */
static void sofa(Scene *s, Kit *k, float x, float z, float yaw, const char *assets) {
    char path[1200], err[256];
    snprintf(path, sizeof(path), "%s/models/sofa.glb", assets);
    Model m;
    if (!model_read(path, &m, err, sizeof(err))) {
        fprintf(stderr, "sofa: %s (a box stands in)\n", err);
        box_local(s, k->sofa, 0, x, z, yaw, 0, 0.36f, 0, 2.2f, 0.72f, 0.9f, 0);
        return;
    }
    snprintf(path, sizeof(path), "%s/materials/%s.json", assets, PBR_MATERIAL_NAMES[m.pbr[0]]);
    MaterialDef def;
    if (material_def_load(path, &def, err, sizeof(err)))
        for (int i = 0; i < 3; ++i) s->mats[k->sofa].albedo[i] = fminf(def.albedoMean[i] * def.albedoScale, 1.0f);
    float *v = malloc(sizeof(float) * 3 * m.nv);
    if (!v) fatal("out of memory reading the sofa");
    for (uint32_t i = 0; i < m.nv; ++i) memcpy(v + i * 3, m.v[i].p, sizeof(float) * 3);
    int id = mesh_add(s, v, m.nv, m.ix, m.ni);
    free(v);
    model_free(&m);
    inst_add(s, id, k->sofa, 0, x, 0.0f, z, 1, 1, 1, yaw);
}

static void lounge(Scene *s, Kit *k, float x, float z, const char *assets, uint32_t seed) {
    box(s, k->rug, 0, x - 1.4f, 0.0f, z - 1.1f, x + 1.4f, 0.012f, z + 1.1f);
    sofa(s, k, x, z + 0.6f, 3.14159265f, assets);   /* its back to the room, facing the glass */
    box(s, k->marble, 0, x + 1.45f, 0.0f, z + 0.35f, x + 1.95f, 0.5f, z + 0.85f);
    box(s, k->marble, 0, x - 0.45f, 0.0f, z - 0.55f, x + 0.45f, 0.38f, z - 0.15f);   /* a low table */
    box(s, k->orb, BF_EMITTER, x - 1.9f, 0.0f, z + 0.4f, x - 1.55f, 0.35f, z + 0.75f);
    light_add(s, x - 1.72f, 0.9f, z + 0.58f, 1.6f, 1.15f, 0.65f);
    plant(s, k, x + 2.6f, z - 0.3f, PS_KENTIA, 1.9f, seed);
}

/* A glass meeting room over [xa, xb] x [za, zb]: glass to 2.7 m, a bulkhead above, a manifestation band, a table. */
static void meeting_room(Scene *s, const Kit *k, float xa, float xb, float za, float zb, uint32_t *rng) {
    float g = 0.006f, top = 2.7f, c = s->ceilY;
    box(s, k->glass, BF_GLASS, xa, 0.0f, za - g, xb, top, za + g);
    box(s, k->glass, BF_GLASS, xa, 0.0f, zb - g, xb, top, zb + g);
    box(s, k->glass, BF_GLASS, xa - g, 0.0f, za, xa + g, top, zb);
    box(s, k->glass, BF_GLASS, xb - g, 0.0f, za, xb + g, top, zb);
    box(s, k->coreWall, 0, xa - 0.05f, top, za - 0.05f, xb + 0.05f, c, za + 0.05f);
    box(s, k->coreWall, 0, xa - 0.05f, top, zb - 0.05f, xb + 0.05f, c, zb + 0.05f);
    box(s, k->coreWall, 0, xa - 0.05f, top, za, xa + 0.05f, c, zb);
    box(s, k->coreWall, 0, xb - 0.05f, top, za, xb + 0.05f, c, zb);
    box(s, k->band, 0, xa, 1.0f, za - g - 0.003f, xb, 1.1f, za - g);
    for (float x = xa; x <= xb + 0.01f; x += (xb - xa) / 4.0f) box(s, k->mullion, 0, x - 0.025f, 0.0f, za - 0.03f, x + 0.025f, top, za + 0.03f);
    float tx0 = xa + 1.0f, tx1 = xb - 1.0f, tz0 = za + 1.3f, tz1 = zb - 1.3f;
    box(s, k->wood, 0, tx0, 0.72f, tz0, tx1, 0.76f, tz1);
    box(s, k->frame, 0, tx0 + 0.4f, 0.0f, (tz0 + tz1) * 0.5f - 0.1f, tx1 - 0.4f, 0.72f, (tz0 + tz1) * 0.5f + 0.1f);
    for (float x = tx0 + 0.5f; x < tx1 - 0.2f; x += 1.0f) {
        chair(s, k, x, tz0 - 0.45f, (rnd(rng) - 0.5f) * 0.5f);
        chair(s, k, x, tz1 + 0.45f, 3.14159265f + (rnd(rng) - 0.5f) * 0.5f);
    }
    pendant(s, k, (tx0 + tx1) * 0.5f, (tz0 + tz1) * 0.5f, true);
}

/* ------------------------------------------------------------ the scenes --- */

static void defaults(Scene *s, const char *name, int type, float W, float D) {
    snprintf(s->name, sizeof(s->name), "%s", name);
    s->type = type;
    s->W = W; s->H = 4.0f; s->D = D;
    s->ceilY = 3.4f;
    s->sill = 0.05f; s->head = 3.35f;
    s->period = W;
    s->xmin = -1e9f; s->xmax = 1e9f;
    s->outside[0] = 0.06f; s->outside[1] = 0.05f; s->outside[2] = 0.09f;
    s->ambient = 0.02f;
}

bool scene_build(Scene *s, const char *name, const char *assets) {
    memset(s, 0, sizeof(*s));
    cube_mesh(s);
    Kit k = kit_make(s);
    uint32_t rng = 0x9E3779B9u;
    float W;

    if (!strcmp(name, "through30")) {
        defaults(s, name, MOD_THROUGH, 12.0f, 30.0f);
        W = s->W;
        slabs(s, &k, -0.5f, W + 0.5f);
        const float colZ[] = { 4.5f, 12.0f, 19.5f, 27.0f }, ductX[] = { 3.0f, 9.0f }, trayZ[] = { 9.0f, 16.5f, 24.0f };
        for (int i = 0; i < 4; ++i) { column(s, &k, 0.0f, colZ[i]); column(s, &k, 6.0f, colZ[i]); }
        services(s, &k, -0.5f, W + 0.5f, colZ, 4, ductX, 2, trayZ, 3, s->D);
        const float xs[] = { 0.6f, 6.6f };
        const int n[] = { 3, 3 };
        const float rows[] = { 6.35f, 10.15f, 13.85f, 17.65f, 21.35f, 25.15f };
        for (int i = 0; i < 6; ++i) desk_rows(s, &k, rows[i], xs, n, 2, &rng);
        far_glass(s, &k, 0.0f, W, s->D);
    } else if (!strcmp(name, "core12")) {
        defaults(s, name, MOD_CORE, 12.0f, 12.5f);
        W = s->W;
        slabs(s, &k, -0.5f, W + 0.5f);
        const float colZ[] = { 6.0f }, ductX[] = { 3.0f, 9.0f }, trayZ[] = { 8.5f };
        column(s, &k, 0.0f, 6.0f);
        column(s, &k, 6.0f, 6.0f);
        services(s, &k, -0.5f, W + 0.5f, colZ, 1, ductX, 2, trayZ, 1, 12.0f);
        const float xs[] = { 0.6f, 6.6f };
        const int n[] = { 3, 3 };
        desk_rows(s, &k, 3.2f, xs, n, 2, &rng);
        desk_rows(s, &k, 9.0f, xs, n, 2, &rng);
        core_wall(s, &k, -0.5f, W + 0.5f, 12.0f, &rng);
    } else if (!strcmp(name, "corner16")) {
        defaults(s, name, MOD_CORNER, 12.0f, 16.5f);
        s->period = 0.0f;
        s->xmin = 0.0f;
        float X = 36.0f;
        slabs(s, &k, 0.0f, X + 1.5f);
        box(s, k.coreWall, 0, X, -0.1f, -2.0f, X + 1.5f, s->ceilY + 0.1f, s->D + 2.0f);
        const float colZ[] = { 5.5f, 11.0f }, ductX[] = { 3.0f, 9.0f, 15.0f, 21.0f, 27.0f, 33.0f }, trayZ[] = { 8.25f, 13.6f };
        for (int c = 1; c < 6; ++c) for (int i = 0; i < 2; ++i) column(s, &k, 6.0f * (float)c, colZ[i]);
        services(s, &k, 0.0f, X, colZ, 2, ductX, 6, trayZ, 2, 16.0f);
        const float xs[] = { 0.8f, 6.6f, 12.6f, 18.6f, 24.6f, 30.6f };
        const int n[] = { 3, 3, 3, 3, 3, 3 };
        desk_rows(s, &k, 8.25f, xs, n, 6, &rng);
        desk_rows(s, &k, 13.6f, xs, n, 6, &rng);
        for (float z = 0.0f; z < 16.0f; z += 1.5f) box(s, k.mullion, 0, 0.02f, 0.0f, z - 0.03f, 0.12f, s->ceilY, z + 0.03f);
        box(s, k.mullion, 0, 0.02f, 0.0f, 0.0f, 0.12f, 0.08f, 16.0f);
        lounge(s, &k, 3.2f, 2.4f, assets, 7u);
        core_wall(s, &k, 0.0f, X, 16.0f, &rng);
    } else if (!strcmp(name, "glass20")) {
        defaults(s, name, MOD_THROUGH, 12.0f, 20.0f);
        W = s->W;
        slabs(s, &k, -0.5f, W + 0.5f);
        const float colZ[] = { 5.0f, 16.0f }, ductX[] = { 3.0f, 9.0f }, trayZ[] = { 8.6f, 15.0f };
        for (int i = 0; i < 2; ++i) { column(s, &k, 0.0f, colZ[i]); column(s, &k, 6.0f, colZ[i]); }
        services(s, &k, -0.5f, W + 0.5f, colZ, 2, ductX, 2, trayZ, 2, s->D);
        meeting_room(s, &k, 0.8f, 5.6f, 9.6f, 14.2f, &rng);
        meeting_room(s, &k, 6.6f, 11.2f, 9.6f, 14.2f, &rng);
        const float xs[] = { 0.6f, 6.6f };
        const int n[] = { 3, 3 };
        desk_rows(s, &k, 7.2f, xs, n, 2, &rng);
        desk_rows(s, &k, 18.0f, xs, n, 2, &rng);
        lounge(s, &k, 2.6f, 2.6f, assets, 3u);
        plant(s, &k, 10.5f, 1.2f, PS_LYRATA, 1.6f, 11u);
        far_glass(s, &k, 0.0f, W, s->D);
    } else {
        fprintf(stderr, "unknown scene %s (", name);
        for (int i = 0; i < SCENE_COUNT; ++i) fprintf(stderr, "%s%s", i ? ", " : "", SCENE_NAMES[i]);
        fprintf(stderr, ")\n");
        scene_free(s);
        return false;
    }
    if (s->type == MOD_CORNER) s->xmax = 1e9f;
    printf("scene %s: %d instances, %d meshes, %u triangles, %d lights\n", s->name, s->ninsts, s->nmeshes, s->nidx / 3, s->nlights);
    return true;
}

void scene_free(Scene *s) {
    free(s->verts); free(s->idx); free(s->meshes); free(s->mats); free(s->insts); free(s->lights);
    memset(s, 0, sizeof(*s));
}
