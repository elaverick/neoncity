#include "bake.h"
#include "kit.h"
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
 * Each material has a roughness, and some a metal's or a clearcoat's sheen
 * (kit_make), for the path tracer. Its lights are its emissive surfaces: the
 * diffusers under the pendants' and lamps' dark housings, the orb lamps and
 * the screens. The point lights beside them stand in for them under IM1's
 * direct light, which the lab uses: a diffuser or an orb emits as brightly as
 * its point light shines straight down (intensity over the face's area).
 *
 *   through30  12 m wide, 30 m deep to far glass: the deep open floor
 *   core12     12 m deep to a core wall with doors
 *   corner16   a corner: glass along the left side too, 16 m to a core wall
 *   glass20    20 m through, with two glass meeting rooms and a lounge
 */

const char *const SCENE_NAMES[] = { "through30", "core12", "corner16", "glass20" };
const int SCENE_COUNT = 4;
const char *const LIGHT_STATE_NAMES[LIGHTS_COUNT] = { "after_hours", "late_shift", "cleaners", "screens" };
const char *const LIGHT_STATE_SUFFIX[LIGHTS_COUNT] = { "", "_late", "_cleaners", "_dark" };

/* ------------------------------------------------------------ the scenes --- */

void scene_defaults(Scene *s, const char *name, int type, float W, float D) {
    snprintf(s->name, sizeof(s->name), "%s", name);
    s->type = type;
    s->W = W; s->H = 4.0f; s->D = D;
    s->ceilY = 3.4f;
    s->sill = 0.05f; s->head = 3.35f;
    s->period = W;
    s->xmin = -1e9f; s->xmax = 1e9f;
    s->outside[0] = 0.06f; s->outside[1] = 0.05f; s->outside[2] = 0.09f;
    s->ambient = 0.02f;
    s->bays = (int)lroundf(W / 3.0f);
    s->bayW = 3.0f;
}

/* The depth a scene is built to, for where its late shift's cluster sits. */
static float scene_depth(const char *name) {
    return !strcmp(name, "through30") ? 30.0f : !strcmp(name, "glass20") ? 20.0f : !strcmp(name, "corner16") ? 16.5f : 12.5f;
}

void scene_calibration(Scene *s, float tableY) {
    scene_begin(s, "calibration");
    const float W = 14.4f, D = 28.8f, H = 4.2f;
    s->W = W; s->H = H + 0.6f; s->D = D; s->ceilY = H;
    s->period = W;
    s->sealed = 1;
    const float none[3] = { 0, 0, 0 }, wall[3] = { 0.4f, 0.38f, 0.36f }, ceil_[3] = { 0.045f, 0.044f, 0.042f }, stone[3] = { 0.22f, 0.21f, 0.20f }, table[3] = { 0.5f, 0.5f, 0.5f };
    float c[3], L = 6000.0f / (PI_F * 1.2f * 1.2f) * BAKE_NIT;   /* the same output a square metre as the office's panels */
    kelvin_unit(3000.0f, c);
    float e[3] = { c[0] * L, c[1] * L, c[2] * L };
    int mw = scene_material(s, wall, none, 0.8f, 0.0f, 0.04f, 0.0f, 0.0f), mc = scene_material(s, ceil_, none, 0.9f, 0.0f, 0.04f, 0.0f, 0.0f);
    int mf = scene_material(s, stone, none, 0.6f, 0.0f, 0.04f, 0.0f, 0.0f), mt = scene_material(s, table, none, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f);
    int ml = scene_material(s, wall, e, 0.8f, 0.0f, 0.04f, 0.0f, 0.0f);
    scene_box(s, mf, 0, -0.5f, -1.0f, -1.0f, W + 0.5f, 0.0f, D + 1.0f);
    scene_box(s, mc, 0, -0.5f, H, -1.0f, W + 0.5f, H + 1.0f, D + 1.0f);
    scene_box(s, mw, 0, -0.5f, -1.0f, -1.0f, W + 0.5f, H + 1.0f, 0.0f);
    scene_box(s, mw, 0, -0.5f, -1.0f, D, W + 0.5f, H + 1.0f, D + 1.0f);
    for (float z = 1.8f; z < D; z += 3.6f)
        for (float x = 1.8f; x < W; x += 3.6f)
            scene_box(s, ml, BF_EMITTER, x - 0.6f, H - 0.01f, z - 0.6f, x + 0.6f, H, z + 0.6f);
    /* a sheet the camera reads, a table's height over the floor, between four fittings */
    scene_box(s, mt, 0, W * 0.5f - 0.5f, tableY - 0.02f, D * 0.5f - 0.5f, W * 0.5f + 0.5f, tableY, D * 0.5f + 0.5f);
}

bool scene_build(Scene *s, const char *name, int state, const char *assets) {
    bool isFloor = false;
    if (floor_scene_build(s, name, state, assets, &isFloor)) return true;
    if (isFloor) return false;
    if (state < 0) state = LIGHTS_AFTER_HOURS;
    memset(s, 0, sizeof(*s));
    cube_mesh(s);
    Kit k = kit_make(s, state, 12.0f, scene_depth(name));
    uint32_t rng = 0x9E3779B9u;
    float W;

    if (!strcmp(name, "through30")) {
        scene_defaults(s, name, MOD_THROUGH, 12.0f, 30.0f);
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
        scene_defaults(s, name, MOD_CORE, 12.0f, 12.5f);
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
        scene_defaults(s, name, MOD_CORNER, 12.0f, 16.5f);
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
        scene_defaults(s, name, MOD_THROUGH, 12.0f, 20.0f);
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
    s->state = state;
    size_t len = strlen(s->name);
    snprintf(s->name + len, sizeof(s->name) - len, "%s", LIGHT_STATE_SUFFIX[state]);
    printf("scene %s: %d instances, %d meshes, %u triangles, %d lights\n", s->name, s->ninsts, s->nmeshes, s->nidx / 3, s->nlights);
    return true;
}

/* FNV-1a over what a bake sees of the scene. */
static uint64_t fnv(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = p;
    for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}

uint64_t scene_hash(const Scene *s) {
    uint64_t h = 14695981039346656037ull;
    h = fnv(h, s->verts, sizeof(float) * 3 * s->nverts);
    h = fnv(h, s->idx, sizeof(uint32_t) * s->nidx);
    h = fnv(h, s->meshes, sizeof(BakeMesh) * (size_t)s->nmeshes);
    h = fnv(h, s->mats, sizeof(BakeMat) * (size_t)s->nmats);
    h = fnv(h, s->insts, sizeof(BakeInst) * (size_t)s->ninsts);
    return h;
}

void scene_free(Scene *s) {
    free(s->verts); free(s->idx); free(s->meshes); free(s->mats); free(s->insts); free(s->lights);
    memset(s, 0, sizeof(*s));
}
