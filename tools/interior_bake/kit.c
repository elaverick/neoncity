#include "kit.h"
#include "models.h"
#include "material_def.h"
#include "plant.h"

/* The kit of parts office floors are built from: the scene's primitives, its materials and lights,
 * and the furniture and services both the test modules (scene.c) and the floor generator's
 * (floor_scene.c) stand on the floor. */

/* The kit's lights are photometric (section 16): lumens and a colour
 * temperature. A surface that emits as Lambert's law says has luminance
 * lumens / (pi * area), in candelas a square metre. The bake's radiance is
 * BAKE_NIT of that, which keeps its numbers near 1 (the clamp's and the
 * lab's), and each module's bake scale takes it to the engine's. */

/* The colour at a temperature, as kelvin() in shaders/city/rooms.glsl, at luminance 1. */
void kelvin_unit(float k, float c[3]) {
    static const float T[6] = { 2700, 3000, 3500, 4000, 5000, 6500 };
    static const float C[6][3] = { { 1.00f, 0.62f, 0.32f }, { 1.00f, 0.69f, 0.42f }, { 1.00f, 0.77f, 0.55f },
                                   { 1.00f, 0.84f, 0.68f }, { 1.00f, 0.93f, 0.85f }, { 0.93f, 0.97f, 1.00f } };
    int i = 0;
    while (i < 4 && k >= T[i + 1]) i++;
    float t = fminf(fmaxf((k - T[i]) / (T[i + 1] - T[i]), 0.0f), 1.0f), lum = 0.0f;
    static const float W[3] = { 0.2126f, 0.7152f, 0.0722f };
    for (int r = 0; r < 3; ++r) { c[r] = C[i][r] + (C[i + 1][r] - C[i][r]) * t; lum += W[r] * c[r]; }
    for (int r = 0; r < 3; ++r) c[r] /= lum;
}

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

int mesh_add(Scene *s, const float *v, uint32_t nv, const uint32_t *ix, uint32_t ni) {
    return mesh_add_attr(s, v, NULL, nv, ix, ni);
}

/* attr: nx, ny, nz, u, v per vertex, or NULL. */
int mesh_add_attr(Scene *s, const float *v, const float *attr, uint32_t nv, const uint32_t *ix, uint32_t ni) {
    GROW(s->verts, s->nverts * 3, s->capVerts, nv * 3);
    GROW(s->attrs, s->nverts * 5, s->capAttrs, nv * 5);
    GROW(s->idx, s->nidx, s->capIdx, ni);
    GROW(s->meshes, s->nmeshes, s->capMeshes, 1);
    BakeMesh *m = &s->meshes[s->nmeshes];
    m->firstIndex = s->nidx; m->indexCount = ni; m->baseVertex = s->nverts; m->vertexCount = nv;
    m->attrs = attr != NULL;
    for (int r = 0; r < 3; ++r) { m->mn[r] = 1e30f; m->mx[r] = -1e30f; }
    for (uint32_t i = 0; i < nv; ++i)
        for (int r = 0; r < 3; ++r) {
            m->mn[r] = fminf(m->mn[r], v[i * 3 + r]);
            m->mx[r] = fmaxf(m->mx[r], v[i * 3 + r]);
        }
    memcpy(s->verts + s->nverts * 3, v, sizeof(float) * 3 * nv);
    if (attr) memcpy(s->attrs + s->nverts * 5, attr, sizeof(float) * 5 * nv);
    else memset(s->attrs + s->nverts * 5, 0, sizeof(float) * 5 * nv);
    memcpy(s->idx + s->nidx, ix, sizeof(uint32_t) * ni);
    s->nverts += nv;
    s->nidx += ni;
    return s->nmeshes++;
}

/* A dielectric of roughness 0.7 (F0 0.04); mat_surface changes that. */
int mat_add(Scene *s, float r, float g, float b, float er, float eg, float eb) {
    GROW(s->mats, s->nmats, s->capMats, 1);
    BakeMat *m = &s->mats[s->nmats];
    memset(m, 0, sizeof(*m));
    m->albedo[0] = r; m->albedo[1] = g; m->albedo[2] = b; m->albedo[3] = 0.7f;
    m->emission[0] = er; m->emission[1] = eg; m->emission[2] = eb;
    m->surface[0] = 0.04f;
    m->tex[0] = m->tex[1] = m->tex[2] = m->tex[3] = -1;
    return s->nmats++;
}

void mat_surface(Scene *s, int id, float rough, float metal, float coat, float coatRough) {
    BakeMat *m = &s->mats[id];
    m->albedo[3] = rough;
    m->emission[3] = metal;
    m->surface[1] = coat;
    m->surface[2] = coatRough;
}

void light_add(Scene *s, float x, float y, float z, float r, float g, float b) {
    GROW(s->lights, s->nlights, s->capLights, 1);
    BakeLight *l = &s->lights[s->nlights++];
    memset(l, 0, sizeof(*l));
    l->pos[0] = x; l->pos[1] = y; l->pos[2] = z;
    l->power[0] = r; l->power[1] = g; l->power[2] = b;
}

/* An instance of mesh scaled by (sx, sy, sz), turned by yaw about y, centred at c. */
void inst_add(Scene *s, int mesh, int mat, uint32_t flags, float cx, float cy, float cz,
                     float sx, float sy, float sz, float yaw) {
    GROW(s->insts, s->ninsts, s->capInsts, 1);
    BakeInst *in = &s->insts[s->ninsts++];
    float c = cosf(yaw), n = sinf(yaw);
    in->mesh = (uint32_t)mesh; in->mat = (uint32_t)mat; in->flags = flags;
    float xf[12] = { c * sx, 0, n * sz, cx,   0, sy, 0, cy,   -n * sx, 0, c * sz, cz };
    memcpy(in->xf, xf, sizeof(xf));
}

void box(Scene *s, int mat, uint32_t flags, float x0, float y0, float z0, float x1, float y1, float z1) {
    inst_add(s, s->cube, mat, flags, (x0 + x1) * 0.5f, (y0 + y1) * 0.5f, (z0 + z1) * 0.5f, x1 - x0, y1 - y0, z1 - z0, 0.0f);
}

/* A box at (lx, ly, lz) in a frame at (ox, oz) turned by yaw. */
void box_local(Scene *s, int mat, uint32_t flags, float ox, float oz, float yaw,
                      float lx, float ly, float lz, float sx, float sy, float sz, float spin) {
    float c = cosf(yaw), n = sinf(yaw);
    inst_add(s, s->cube, mat, flags, ox + c * lx + n * lz, ly, oz - n * lx + c * lz, sx, sy, sz, yaw + spin);
}

void cube_mesh(Scene *s) {
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

void scene_begin(Scene *s, const char *name) {
    memset(s, 0, sizeof(*s));
    snprintf(s->name, sizeof(s->name), "%s", name);
    cube_mesh(s);
    s->xmin = -1e9f; s->xmax = 1e9f;
}

int scene_material(Scene *s, const float albedo[3], const float emission[3], float rough, float metal, float f0, float coat, float coatRough) {
    int id = mat_add(s, albedo[0], albedo[1], albedo[2], emission[0], emission[1], emission[2]);
    mat_surface(s, id, rough, metal, coat, coatRough);
    s->mats[id].surface[0] = f0;
    return id;
}

void scene_box(Scene *s, int mat, uint32_t flags, float x0, float y0, float z0, float x1, float y1, float z1) {
    box(s, mat, flags, x0, y0, z0, x1, y1, z1);
}

/* ------------------------------------------------------------- the kit --- */


/* A material emitting lumens over area (the face that lights the room) at a colour temperature. */
int mat_light(Scene *s, float lumens, float area, float kelvin, float share) {
    float c[3], L = lumens / (PI_F * area) * BAKE_NIT * share;
    kelvin_unit(kelvin, c);
    return mat_add(s, 0.8f, 0.8f, 0.8f, c[0] * L, c[1] * L, c[2] * L);
}

Kit kit_make(Scene *s, int state, float W, float D) {
    Kit k;
    k.state = state;
    k.cx = W * 0.3f; k.cz = D * 0.45f; k.cr = 6.5f;
    k.floor      = mat_add(s, 0.16f, 0.155f, 0.15f, 0, 0, 0);    /* polished concrete */
    k.ceiling    = mat_add(s, 0.07f, 0.07f, 0.075f, 0, 0, 0);    /* painted-out exposed slab */
    k.concrete   = mat_add(s, 0.13f, 0.125f, 0.115f, 0, 0, 0);
    k.duct       = mat_add(s, 0.32f, 0.33f, 0.35f, 0, 0, 0);
    k.tray       = mat_add(s, 0.22f, 0.22f, 0.23f, 0, 0, 0);
    k.pipe       = mat_add(s, 0.35f, 0.05f, 0.04f, 0, 0, 0);     /* sprinkler red */
    k.wood       = mat_add(s, 0.07f, 0.05f, 0.035f, 0, 0, 0);
    k.frame      = mat_add(s, 0.03f, 0.03f, 0.03f, 0, 0, 0);
    k.pedestal   = mat_add(s, 0.045f, 0.045f, 0.05f, 0, 0, 0);
    k.mesh       = mat_add(s, 0.025f, 0.025f, 0.028f, 0, 0, 0);
    k.plastic    = mat_add(s, 0.02f, 0.02f, 0.022f, 0, 0, 0);
    k.screenOn   = mat_add(s, 0.02f, 0.02f, 0.02f, 1.0f, 1.0f, 1.0f);    /* radiance 1: BASIS_SCREENS, which the engine colours */
    s->mats[k.screenOn].surface[3] = (float)GROUP_SCREEN;
    k.screenOff  = mat_add(s, 0.01f, 0.01f, 0.012f, 0, 0, 0);
    k.pendantOn  = mat_light(s, LM_PENDANT, 1.56f * 0.1f, 2700.0f, 1.0f);     /* the diffuser, 1.56 x 0.1 m */
    k.pendantUp  = mat_light(s, LM_PENDANT, 1.56f * 0.1f, 2700.0f, 0.4f);     /* its uplight: 40% of the diffuser's, onto the ceiling */
    k.pendantOff = mat_add(s, 0.05f, 0.05f, 0.05f, 0, 0, 0);
    k.lampOn     = mat_light(s, LM_LAMP, 0.18f * 0.06f, 2700.0f, 1.0f);     /* the diffuser, 0.18 x 0.06 m */
    k.lampOff    = mat_add(s, 0.05f, 0.04f, 0.03f, 0, 0, 0);
    k.wire       = mat_add(s, 0.02f, 0.02f, 0.02f, 0, 0, 0);
    k.mullion    = mat_add(s, 0.04f, 0.04f, 0.045f, 0, 0, 0);
    k.coreWall   = mat_add(s, 0.16f, 0.15f, 0.14f, 0, 0, 0);
    k.door       = mat_add(s, 0.10f, 0.07f, 0.05f, 0, 0, 0);
    k.board      = mat_add(s, 0.10f, 0.09f, 0.075f, 0, 0, 0);
    k.glass      = mat_add(s, 0.0f, 0.0f, 0.0f, 0, 0, 0);
    k.band       = mat_add(s, 0.60f, 0.60f, 0.60f, 0, 0, 0);    /* frosted manifestation */
    k.rug        = mat_add(s, 0.16f, 0.09f, 0.06f, 0, 0, 0);
    k.marble     = mat_add(s, 0.05f, 0.045f, 0.04f, 0, 0, 0);    /* black, as the reference's */
    k.orb        = mat_light(s, LM_ORB, 4.0f * PI_F * 0.17f * 0.17f, 2700.0f, 1.0f);   /* a globe of 17 cm radius */
    k.orbOff     = mat_add(s, 0.6f, 0.58f, 0.55f, 0, 0, 0);
    k.trofferOn  = mat_light(s, LM_TROFFER, 0.6f * 1.2f, 4000.0f, 1.0f);      /* the ceiling's panels, 0.6 x 1.2 m: on only for the cleaners */
    k.trofferOff = mat_add(s, 0.5f, 0.5f, 0.5f, 0, 0, 0);
    k.stripOn    = mat_light(s, LM_STRIP, 1.4f * 0.015f, 3000.0f, 1.0f);     /* the strip under a desk's back edge */
    k.coveOn     = mat_light(s, LM_COVE_M, 1.0f * 0.02f, 3000.0f, 1.0f);      /* a cove strip, 300 lumens a metre */
    k.pot        = mat_add(s, 0.06f, 0.06f, 0.065f, 0, 0, 0);
    k.leaf       = mat_add(s, 0.05f, 0.11f, 0.035f, 0, 0, 0);
    k.bark       = mat_add(s, 0.12f, 0.08f, 0.05f, 0, 0, 0);
    k.sofa       = mat_add(s, 0.10f, 0.08f, 0.07f, 0, 0, 0);    /* replaced by the model's material if it loads */
    /* The path tracer's surfaces (pathtrace.glsl): roughness, metallic, clearcoat and its roughness. The
     * rest keep mat_add's matt dielectric. */
    mat_surface(s, k.floor, 0.55f, 0.0f, 1.0f, 0.12f);   /* sealed and polished: it mirrors the lights in long streaks */
    mat_surface(s, k.ceiling, 0.9f, 0.0f, 0.0f, 0.0f);
    mat_surface(s, k.concrete, 0.85f, 0.0f, 0.0f, 0.0f);
    mat_surface(s, k.duct, 0.45f, 1.0f, 0.0f, 0.0f);     /* galvanised */
    mat_surface(s, k.tray, 0.5f, 1.0f, 0.0f, 0.0f);
    mat_surface(s, k.pipe, 0.4f, 0.0f, 0.0f, 0.0f);
    mat_surface(s, k.wood, 0.35f, 0.0f, 0.0f, 0.0f);
    mat_surface(s, k.frame, 0.45f, 0.0f, 0.0f, 0.0f);
    mat_surface(s, k.pedestal, 0.45f, 0.0f, 0.0f, 0.0f);
    mat_surface(s, k.mesh, 0.85f, 0.0f, 0.0f, 0.0f);
    mat_surface(s, k.plastic, 0.4f, 0.0f, 0.0f, 0.0f);
    mat_surface(s, k.screenOn, 0.1f, 0.0f, 0.0f, 0.0f);
    mat_surface(s, k.screenOff, 0.1f, 0.0f, 0.0f, 0.0f);
    mat_surface(s, k.mullion, 0.35f, 0.0f, 0.0f, 0.0f);
    mat_surface(s, k.coreWall, 0.85f, 0.0f, 0.0f, 0.0f);
    mat_surface(s, k.door, 0.45f, 0.0f, 0.0f, 0.0f);
    mat_surface(s, k.rug, 1.0f, 0.0f, 0.0f, 0.0f);
    mat_surface(s, k.marble, 0.15f, 0.0f, 0.0f, 0.0f);
    mat_surface(s, k.pot, 0.4f, 0.0f, 0.0f, 0.0f);
    mat_surface(s, k.leaf, 0.45f, 0.0f, 0.0f, 0.0f);
    mat_surface(s, k.bark, 0.8f, 0.0f, 0.0f, 0.0f);
    return k;
}

/* Each fitting's state: on by chance after hours, in the cluster on the late shift. r: a draw. */
bool cluster(const Kit *k, float x, float z) {
    float dx = x - k->cx, dz = z - k->cz;
    return dx * dx + dz * dz < k->cr * k->cr;
}

bool pendant_lit(const Kit *k, float x, float z, float r) {
    return k->state == LIGHTS_AFTER_HOURS ? r < 0.5f : k->state == LIGHTS_LATE_SHIFT ? cluster(k, x, z) : false;
}

bool lamp_lit(const Kit *k, float x, float z, float r) {
    return k->state == LIGHTS_AFTER_HOURS ? r < 0.75f : k->state == LIGHTS_LATE_SHIFT ? cluster(k, x, z) || r < 0.08f : false;
}

bool screen_lit(const Kit *k, float x, float z, float r) {
    switch (k->state) {
    case LIGHTS_AFTER_HOURS: return r < 0.6f;
    case LIGHTS_LATE_SHIFT:  return cluster(k, x, z) ? r < 0.95f : r < 0.1f;
    case LIGHTS_CLEANERS:    return r < 0.08f;
    default:             return r < 0.5f;
    }
}

bool strip_lit(const Kit *k, float x, float z, float r) {
    return k->state == LIGHTS_AFTER_HOURS ? r < 0.5f : k->state == LIGHTS_LATE_SHIFT ? cluster(k, x, z) : false;
}

/* Hung panels between the ducts, on a 3 m by 3.6 m grid: dark unless the cleaners are in. */
void troffers(Scene *s, const Kit *k, float x0, float x1, float zEnd) {
    float y = s->ceilY - 0.6f;
    for (float z = 1.8f; z < zEnd - 0.7f; z += 3.6f)
        for (float x = x0 + 1.5f; x < x1 - 0.5f; x += 3.0f) {
            box(s, k->trofferOff, BF_EMITTER, x - 0.62f, y, z - 0.32f, x + 0.62f, y + 0.08f, z + 0.32f);
            if (k->state == LIGHTS_CLEANERS) box(s, k->trofferOn, BF_EMITTER, x - 0.6f, y - 0.004f, z - 0.3f, x + 0.6f, y, z + 0.3f);
        }
}

void slabs(Scene *s, const Kit *k, float x0, float x1) {
    box(s, k->floor, 0, x0, -2.0f, -2.0f, x1, 0.0f, s->D + 2.0f);
    box(s, k->ceiling, 0, x0, s->ceilY, -2.0f, x1, s->ceilY + 2.6f, s->D + 2.0f);
}

void column(Scene *s, const Kit *k, float x, float z) {
    box(s, k->concrete, 0, x - 0.3f, -0.1f, z - 0.3f, x + 0.3f, s->ceilY + 0.1f, z + 0.3f);
}

/* Downstand beams on the column lines (along x), ducts along z, trays and sprinkler runs along x. */
void services(Scene *s, const Kit *k, float x0, float x1, const float *beamZ, int nbeam, const float *ductX, int nduct,
                     const float *trayZ, int ntray, float zEnd) {
    float c = s->ceilY;
    for (int i = 0; i < nbeam; ++i) box(s, k->concrete, 0, x0, c - 0.45f, beamZ[i] - 0.2f, x1, c + 0.05f, beamZ[i] + 0.2f);
    for (int i = 0; i < nduct; ++i) {
        box(s, k->duct, 0, ductX[i] - 0.35f, c - 0.95f, 0.4f, ductX[i] + 0.35f, c - 0.5f, zEnd - 0.4f);
        for (float z = 3.0f; z < zEnd - 1.0f; z += 6.0f)   /* diffusers */
            box(s, k->duct, 0, ductX[i] - 0.2f, c - 1.05f, z - 0.2f, ductX[i] + 0.2f, c - 0.95f, z + 0.2f);
    }
    troffers(s, k, x0 + 0.5f, x1 - 0.5f, zEnd);
    for (int i = 0; i < ntray; ++i) box(s, k->tray, 0, x0, c - 0.42f, trayZ[i] - 0.18f, x1, c - 0.36f, trayZ[i] + 0.18f);
    for (float z = 1.5f; z < zEnd - 0.5f; z += 3.0f) box(s, k->pipe, 0, x0, c - 0.15f, z - 0.02f, x1, c - 0.11f, z + 0.02f);
}

/* A 1.6 m linear pendant centred at (x, z) on two wires: a dark housing, and when it is on a diffuser
 * under it that lights the desks (and the point light that stands in for it) and a dimmer one on top
 * that washes the ceiling (path traced only). */
void pendant(Scene *s, const Kit *k, float x, float z, bool on) {
    float y = 2.62f;
    box(s, k->pendantOff, BF_EMITTER, x - 0.8f, y, z - 0.06f, x + 0.8f, y + 0.05f, z + 0.06f);
    if (on) {
        box(s, k->pendantOn, BF_EMITTER, x - 0.78f, y - 0.004f, z - 0.05f, x + 0.78f, y, z + 0.05f);
        box(s, k->pendantUp, BF_EMITTER, x - 0.78f, y + 0.05f, z - 0.05f, x + 0.78f, y + 0.054f, z + 0.05f);
    }
    box(s, k->wire, 0, x - 0.7f, y + 0.05f, z - 0.004f, x - 0.692f, s->ceilY, z + 0.004f);
    box(s, k->wire, 0, x + 0.692f, y + 0.05f, z - 0.004f, x + 0.7f, s->ceilY, z + 0.004f);
    if (on) light_add(s, x, y - 0.03f, z, 9.0f, 6.8f, 4.3f);
}

void chair(Scene *s, const Kit *k, float x, float z, float yaw) {
    box_local(s, k->mesh, 0, x, z, yaw, 0, 0.47f, 0, 0.5f, 0.07f, 0.48f, 0);
    box_local(s, k->mesh, 0, x, z, yaw, 0, 0.82f, 0.26f, 0.48f, 0.55f, 0.05f, 0);
    box_local(s, k->frame, 0, x, z, yaw, -0.27f, 0.64f, 0.02f, 0.04f, 0.04f, 0.36f, 0);
    box_local(s, k->frame, 0, x, z, yaw, 0.27f, 0.64f, 0.02f, 0.04f, 0.04f, 0.36f, 0);
    box_local(s, k->frame, 0, x, z, yaw, 0, 0.25f, 0, 0.06f, 0.38f, 0.06f, 0);
    box_local(s, k->frame, 0, x, z, yaw, 0, 0.05f, 0, 0.62f, 0.04f, 0.06f, 0.6f);
    box_local(s, k->frame, 0, x, z, yaw, 0, 0.05f, 0, 0.62f, 0.04f, 0.06f, 2.17f);
}

/* A desk from x to x + 1.6, its back edge at zBack, running 0.8 m in direction dir (+1 or -1), its user beyond. */
void desk(Scene *s, const Kit *k, float x, float zBack, float dir, uint32_t *rng) {
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
        float zs = zBack + dir * 0.18f;
        bool on = screen_lit(k, cx, zs, rnd(rng));
        box(s, on ? k->screenOn : k->screenOff, 0, cx - 0.27f, 0.92f, zs - 0.012f, cx + 0.27f, 1.24f, zs + 0.012f);
        box(s, k->plastic, 0, cx - 0.025f, 0.75f, zs - 0.025f + dir * 0.03f, cx + 0.025f, 0.93f, zs + 0.025f + dir * 0.03f);
        box(s, k->plastic, 0, cx - 0.1f, 0.75f, zs - 0.07f + dir * 0.03f, cx + 0.1f, 0.76f, zs + 0.07f + dir * 0.03f);
    }
    float zk = zBack + dir * 0.5f;
    box(s, k->plastic, 0, x + 0.55f, 0.75f, zk - 0.07f, x + 1.0f, 0.77f, zk + 0.07f);
    float zu = zBack + dir * 0.12f;
    if (strip_lit(k, x + 0.8f, zu, rnd(rng)))   /* the strip under its back edge */
        box(s, k->stripOn, BF_EMITTER, x + 0.1f, 0.712f, zu - 0.0075f, x + 1.5f, 0.716f, zu + 0.0075f);
    if (rnd(rng) < 0.45f) {   /* a task lamp */
        float lx = x + 0.15f, lz = zBack + dir * 0.15f;
        bool on = lamp_lit(k, lx, lz, rnd(rng));
        box(s, k->plastic, 0, lx - 0.06f, 0.75f, lz - 0.06f, lx + 0.06f, 0.77f, lz + 0.06f);
        box(s, k->plastic, 0, lx - 0.01f, 0.77f, lz - 0.01f, lx + 0.01f, 1.17f, lz + 0.01f);
        box(s, k->lampOff, BF_EMITTER, lx - 0.02f, 1.12f, lz - 0.04f, lx + 0.2f, 1.17f, lz + 0.04f);
        if (on) box(s, k->lampOn, BF_EMITTER, lx, 1.116f, lz - 0.03f, lx + 0.18f, 1.12f, lz + 0.03f);
        if (on) light_add(s, lx + 0.12f, 1.1f, lz, 0.9f, 0.52f, 0.22f);
    }
    float jitter = (rnd(rng) - 0.5f) * 0.4f;
    float yaw = (dir > 0 ? 0.0f : 3.14159265f) + (rnd(rng) - 0.5f) * 0.9f;
    chair(s, k, x + 0.8f + jitter, zBack + dir * 1.15f, yaw);
}

/* Desks back to back along zc, in runs of 1.6 m from each xs[i] for n[i] desks, with a pendant over each pair. */
void desk_rows(Scene *s, const Kit *k, float zc, const float *xs, const int *n, int runs, uint32_t *rng) {
    for (int r = 0; r < runs; ++r) {
        float x0 = xs[r], x1 = xs[r] + 1.6f * (float)n[r];
        box(s, k->board, 0, x0 + 0.05f, 0.75f, zc - 0.01f, x1 - 0.05f, 1.15f, zc + 0.01f);   /* the screen between */
        if (strip_lit(k, 0.5f * (x0 + x1), zc, rnd(rng)))   /* a cove along its top */
            box(s, k->coveOn, BF_EMITTER, x0 + 0.05f, 1.15f, zc - 0.01f, x1 - 0.05f, 1.154f, zc + 0.01f);
        for (int i = 0; i < n[r]; ++i) {
            float x = x0 + 1.6f * (float)i;
            desk(s, k, x, zc - 0.01f, -1.0f, rng);
            desk(s, k, x, zc + 0.01f, 1.0f, rng);
            pendant(s, k, x + 0.8f, zc, pendant_lit(k, x + 0.8f, zc, rnd(rng)));
        }
    }
}

/* Mullions and transoms of glass at depth z (the far facade). */
void far_glass(Scene *s, const Kit *k, float x0, float x1, float z) {
    for (float x = x0; x < x1 - 0.01f; x += 1.5f) box(s, k->mullion, 0, x - 0.03f, 0.0f, z - 0.12f, x + 0.03f, s->ceilY, z - 0.02f);
    box(s, k->mullion, 0, x0 - 0.5f, 0.0f, z - 0.12f, x1 + 0.5f, 0.08f, z - 0.02f);
    box(s, k->mullion, 0, x0 - 0.5f, s->ceilY - 0.1f, z - 0.12f, x1 + 0.5f, s->ceilY, z - 0.02f);
}

void core_wall(Scene *s, const Kit *k, float x0, float x1, float z, uint32_t *rng) {
    box(s, k->coreWall, 0, x0, -0.1f, z, x1, s->ceilY + 0.1f, z + 2.0f);
    for (float x = x0 + 1.5f + rnd(rng) * 2.0f; x < x1 - 2.0f; x += 4.0f + rnd(rng) * 3.0f) {
        box(s, k->door, 0, x, 0.0f, z - 0.03f, x + 0.95f, 2.15f, z + 0.01f);
        box(s, k->frame, 0, x - 0.05f, 0.0f, z - 0.05f, x, 2.2f, z + 0.01f);
        box(s, k->frame, 0, x + 0.95f, 0.0f, z - 0.05f, x + 1.0f, 2.2f, z + 0.01f);
        box(s, k->frame, 0, x - 0.05f, 2.15f, z - 0.05f, x + 1.0f, 2.2f, z + 0.01f);
    }
    float xb = x0 + (x1 - x0) * 0.55f;
    if (k->state == LIGHTS_AFTER_HOURS || k->state == LIGHTS_LATE_SHIFT)   /* a cove along its head */
        box(s, k->coveOn, BF_EMITTER, x0, s->ceilY - 0.3f, z - 0.02f, x1, s->ceilY - 0.28f, z);
    box(s, k->board, 0, xb, 1.0f, z - 0.02f, xb + 1.2f, 1.9f, z);
    box(s, k->pedestal, 0, xb + 1.6f, 0.0f, z - 0.55f, xb + 2.2f, 1.05f, z);   /* a printer */
}

void plant(Scene *s, const Kit *k, float x, float z, int species, float height, uint32_t seed) {
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
void sofa(Scene *s, Kit *k, float x, float z, float yaw, const char *assets) {
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

/* A globe of radius 1: an octahedron's faces split three times, pushed out to the sphere. */
int orb_mesh(Scene *s) {
    static const float O[6][3] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
    static const int F[8][3] = { { 0, 2, 4 }, { 4, 2, 1 }, { 1, 2, 5 }, { 5, 2, 0 }, { 4, 3, 0 }, { 1, 3, 4 }, { 5, 3, 1 }, { 0, 3, 5 } };
    enum { N = 8, TRIS = 8 * N * N };
    float *v = malloc(sizeof(float) * 9 * TRIS);
    uint32_t *ix = malloc(sizeof(uint32_t) * 3 * TRIS), n = 0;
    if (!v || !ix) fatal("out of memory building an orb");
    for (int f = 0; f < 8; ++f)
        for (int i = 0; i < N; ++i)
            for (int j = 0; j < N - i; ++j)
                for (int up = 0; up < (j < N - i - 1 ? 2 : 1); ++up) {
                    int t[3][2] = { { i, j }, { i + 1, j }, { i, j + 1 } };
                    if (up) { t[0][0] = i + 1; t[0][1] = j + 1; t[1][0] = i; t[1][1] = j + 1; t[2][0] = i + 1; t[2][1] = j; }
                    for (int c = 0; c < 3; ++c) {
                        float a = (float)t[c][0] / N, b = (float)t[c][1] / N, w = 1.0f - a - b, p[3], l = 0.0f;
                        for (int r = 0; r < 3; ++r) { p[r] = w * O[F[f][0]][r] + a * O[F[f][1]][r] + b * O[F[f][2]][r]; l += p[r] * p[r]; }
                        l = sqrtf(l);
                        for (int r = 0; r < 3; ++r) v[n * 3 + r] = p[r] / l;
                        ix[n] = n;
                        n++;
                    }
                }
    int id = mesh_add(s, v, n, ix, n);
    free(v);
    free(ix);
    return id;
}

void lounge(Scene *s, Kit *k, float x, float z, const char *assets, uint32_t seed) {
    box(s, k->rug, 0, x - 1.4f, 0.0f, z - 1.1f, x + 1.4f, 0.012f, z + 1.1f);
    sofa(s, k, x, z + 0.6f, 3.14159265f, assets);   /* its back to the room, facing the glass */
    box(s, k->marble, 0, x + 1.45f, 0.0f, z + 0.35f, x + 1.95f, 0.5f, z + 0.85f);
    box(s, k->marble, 0, x - 0.45f, 0.0f, z - 0.55f, x + 0.45f, 0.38f, z - 0.15f);   /* a low table */
    box(s, k->marble, 0, x - 1.97f, 0.0f, z + 0.33f, x - 1.47f, 0.5f, z + 0.83f);   /* the orb lamp, on its plinth */
    bool lit = k->state == LIGHTS_AFTER_HOURS || k->state == LIGHTS_LATE_SHIFT;
    inst_add(s, orb_mesh(s), lit ? k->orb : k->orbOff, BF_EMITTER, x - 1.72f, 0.67f, z + 0.58f, 0.17f, 0.17f, 0.17f, 0.0f);
    if (lit) light_add(s, x - 1.72f, 0.9f, z + 0.58f, 1.6f, 1.15f, 0.65f);
    plant(s, k, x + 2.6f, z - 0.3f, PS_KENTIA, 1.9f, seed);
}

/* A glass meeting room over [xa, xb] x [za, zb]: glass to 2.7 m, a bulkhead above, a manifestation band, a table. */
void meeting_room(Scene *s, const Kit *k, float xa, float xb, float za, float zb, uint32_t *rng) {
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
    float px = (tx0 + tx1) * 0.5f, pz = (tz0 + tz1) * 0.5f;
    bool lit = k->state == LIGHTS_AFTER_HOURS || (k->state == LIGHTS_LATE_SHIFT && cluster(k, px, pz));
    pendant(s, k, px, pz, lit);
    if (lit) box(s, k->coveOn, BF_EMITTER, xa, top - 0.02f, za + 0.05f, xb, top, za + 0.07f);   /* a cove along its head */
}
